#ifndef SUB_REACTOR_H
#define SUB_REACTOR_H

#include <pthread.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>
#include <time.h>
#include "http/http_conn.h"
#include "lock/locker.h"

class WebServer;

class SubReactor
{
public:
    SubReactor(WebServer *server, int id);
    ~SubReactor();

    bool start();
    void stop();
    void enqueue_connection(http_conn *conn, int sockfd, const sockaddr_in &address);
    void enqueue_completion(http_conn *conn, int sockfd, uint64_t generation,
                            http_conn::PROCESS_RESULT result);
    size_t connection_count() const { return m_connection_count; }

private:
    struct pending_connection { http_conn *conn; int sockfd; sockaddr_in address; };
    struct completion_event {
        http_conn *conn; int sockfd; uint64_t generation; http_conn::PROCESS_RESULT result;
    };

    static void *thread_entry(void *arg);
    void run();
    void wake();
    void handle_notifications();
    void handle_read(int sockfd);
    void handle_write(int sockfd);
    void close_connection(int sockfd);
    void refresh_deadline(int sockfd);
    void expire_idle_connections();
    bool dispatch(http_conn *conn);

    WebServer *m_server;
    int m_id;
    int m_epollfd;
    int m_notifyfd;
    pthread_t m_thread;
    std::atomic<bool> m_running;
    std::atomic<size_t> m_connection_count;
    locker m_queue_lock;
    std::vector<pending_connection> m_pending;
    std::vector<completion_event> m_completed;
    std::unordered_map<int, http_conn *> m_connections;
    std::unordered_map<int, time_t> m_deadlines;
};

#endif
