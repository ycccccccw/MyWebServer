#include "sub_reactor.h"
#include "webserver.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>

static const int SUB_REACTOR_IDLE_TIMEOUT = 3 * TIMESLOT;
static const int SUB_REACTOR_WRITE_TIMEOUT = 60;

SubReactor::SubReactor(WebServer *server, int id)
    : m_server(server), m_id(id), m_epollfd(-1), m_notifyfd(-1),
      m_running(false), m_connection_count(0) {}

SubReactor::~SubReactor()
{
    stop();
    if (m_notifyfd != -1) close(m_notifyfd);
    if (m_epollfd != -1) close(m_epollfd);
}

bool SubReactor::start()
{
    m_epollfd = epoll_create1(EPOLL_CLOEXEC);
    m_notifyfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_epollfd == -1 || m_notifyfd == -1) return false;
    epoll_event event = {};
    event.data.fd = m_notifyfd;
    event.events = EPOLLIN;
    if (epoll_ctl(m_epollfd, EPOLL_CTL_ADD, m_notifyfd, &event) == -1) return false;
    m_running.store(true);
    if (pthread_create(&m_thread, nullptr, thread_entry, this) != 0) {
        m_running.store(false);
        return false;
    }
    return true;
}

void SubReactor::stop()
{
    if (!m_running.exchange(false)) return;
    wake();
    pthread_join(m_thread, nullptr);
}

void SubReactor::wake()
{
    uint64_t one = 1;
    ssize_t ignored = write(m_notifyfd, &one, sizeof(one));
    (void)ignored;
}

void SubReactor::enqueue_connection(http_conn *conn, int sockfd, const sockaddr_in &address)
{
    pending_connection item = {conn, sockfd, address};
    m_queue_lock.lock();
    m_pending.push_back(item);
    m_queue_lock.unlock();
    wake();
}

void SubReactor::enqueue_completion(http_conn *conn, int sockfd, uint64_t generation,
                                    http_conn::PROCESS_RESULT result)
{
    completion_event item = {conn, sockfd, generation, result};
    m_queue_lock.lock();
    m_completed.push_back(item);
    m_queue_lock.unlock();
    wake();
}

void *SubReactor::thread_entry(void *arg)
{
    static_cast<SubReactor *>(arg)->run();
    return nullptr;
}

bool SubReactor::dispatch(http_conn *conn)
{
    conn->begin_processing();
    if (m_server->m_pool->append_p(conn)) return true;
    conn->cancel_processing();
    return false;
}

void SubReactor::refresh_deadline(int sockfd, int timeout_seconds)
{
    m_deadlines[sockfd] = time(nullptr) + timeout_seconds;
}

void SubReactor::close_connection(int sockfd)
{
    auto it = m_connections.find(sockfd);
    if (it == m_connections.end()) return;
    if (it->second->defer_timeout()) {
        m_deadlines.erase(sockfd);
        return;
    }
    http_conn *conn = it->second;
    conn->close_conn();
    m_connections.erase(it);
    m_deadlines.erase(sockfd);
    m_pending_writes.erase(sockfd);
    --m_connection_count;
    m_server->recycle_conn(conn);
}

void SubReactor::handle_read(int sockfd)
{
    auto it = m_connections.find(sockfd);
    if (it == m_connections.end()) return;
    http_conn *conn = it->second;
    if (!conn->read_once() || !dispatch(conn)) {
        close_connection(sockfd);
        return;
    }
    refresh_deadline(sockfd, SUB_REACTOR_IDLE_TIMEOUT);
}

void SubReactor::handle_write(int sockfd)
{
    auto it = m_connections.find(sockfd);
    if (it == m_connections.end()) return;
    http_conn *conn = it->second;
    if (!conn->write()) {
        close_connection(sockfd);
        return;
    }
    if (conn->has_pending_write())
        m_pending_writes.insert(sockfd);
    else
        m_pending_writes.erase(sockfd);
    refresh_deadline(sockfd, conn->has_pending_write() ? SUB_REACTOR_WRITE_TIMEOUT
                                                       : SUB_REACTOR_IDLE_TIMEOUT);
    if (conn->has_buffered_request() && !dispatch(conn)) close_connection(sockfd);
}

void SubReactor::handle_notifications()
{
    uint64_t value;
    while (read(m_notifyfd, &value, sizeof(value)) > 0) {}
    std::vector<pending_connection> pending;
    std::vector<completion_event> completed;
    m_queue_lock.lock();
    pending.swap(m_pending);
    completed.swap(m_completed);
    m_queue_lock.unlock();

    for (const pending_connection &item : pending) {
        item.conn->init(item.sockfd, item.address, m_server->m_root, m_server->m_CONNTrigmode,
                        m_server->m_close_log, m_server->m_user, m_server->m_passWord,
                        m_server->m_databaseName, m_epollfd, m_server->m_connPool, m_id);
        m_connections[item.sockfd] = item.conn;
        ++m_connection_count;
        refresh_deadline(item.sockfd, SUB_REACTOR_IDLE_TIMEOUT);
    }

    for (const completion_event &event : completed) {
        auto it = m_connections.find(event.sockfd);
        if (it == m_connections.end() || it->second != event.conn ||
            event.conn->generation() != event.generation) continue;
        if (event.conn->finish_processing()) {
            close_connection(event.sockfd);
        } else if (event.result == http_conn::PROCESS_NEED_READ) {
            event.conn->arm_read();
            refresh_deadline(event.sockfd, SUB_REACTOR_IDLE_TIMEOUT);
        } else if (event.result == http_conn::PROCESS_READY_WRITE) {
            event.conn->arm_write();
            m_pending_writes.insert(event.sockfd);
            refresh_deadline(event.sockfd, SUB_REACTOR_WRITE_TIMEOUT);
        } else {
            close_connection(event.sockfd);
        }
    }
}

void SubReactor::expire_idle_connections()
{
    time_t now = time(nullptr);
    std::vector<int> expired;
    for (const auto &item : m_deadlines)
        if (item.second <= now) expired.push_back(item.first);
    for (int sockfd : expired) close_connection(sockfd);
}

void SubReactor::run()
{
    epoll_event events[1024];
    time_t last_write_retry = 0;
    while (m_running.load()) {
        int count = epoll_wait(m_epollfd, events, 1024, 1000);
        if (count < 0 && errno != EINTR) break;
        for (int i = 0; i < count; ++i) {
            int fd = events[i].data.fd;
            if (fd == m_notifyfd) handle_notifications();
            else if (events[i].events & (EPOLLHUP | EPOLLERR)) close_connection(fd);
            else {
                auto it = m_connections.find(fd);
                bool pending_write = it != m_connections.end() &&
                                     it->second->has_pending_write();
                if (pending_write && (events[i].events & EPOLLOUT))
                    handle_write(fd);
                else if (events[i].events & EPOLLIN)
                    handle_read(fd);
                else if (events[i].events & EPOLLOUT)
                    handle_write(fd);
                else if (events[i].events & EPOLLRDHUP)
                    close_connection(fd);
            }
        }
        time_t now = time(nullptr);
        if (now != last_write_retry) {
            last_write_retry = now;
            std::vector<int> pending_writes(m_pending_writes.begin(), m_pending_writes.end());
            for (int fd : pending_writes)
                if (m_connections.find(fd) != m_connections.end()) handle_write(fd);
        }
        expire_idle_connections();
    }
    std::vector<int> sockets;
    for (const auto &item : m_connections) sockets.push_back(item.first);
    for (int fd : sockets) close_connection(fd);
}
