#include "webserver.h"
#include "sub_reactor.h"
#include <sys/eventfd.h>

WebServer *WebServer::s_instance = nullptr;

WebServer::WebServer(){
    s_instance = this;
    m_max_connections = DEFAULT_MAX_CONNECTIONS;
    m_completionfd = -1;
    m_next_reactor = 0;

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);//获取当前工作目录
    char root[6] = "/root";//root文件夹存放网页资源文件
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);//拼接当前目录和root文件夹
}

WebServer::~WebServer(){
    for (SubReactor *reactor : m_reactors)
        reactor->stop();
    for (SubReactor *reactor : m_reactors)
        delete reactor;
    m_reactors.clear();
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    if (m_completionfd != -1)
        close(m_completionfd);
    for (auto &item : users)
    {
        item.second->close_conn();
    }
    users.clear();
    for (http_conn *conn : m_conn_pool)
    {
        delete conn;
    }
    m_conn_pool.clear();
    m_free_conns.clear();
    users_timer.clear();
    for (client_data *data : m_client_data_pool)
    {
        delete data;
    }
    m_client_data_pool.clear();
    m_free_client_data.clear();
    for (util_timer *timer : m_timer_pool)
    {
        delete timer;
    }
    m_timer_pool.clear();
    m_free_timers.clear();
    delete m_pool;
    if (s_instance == this)
    {
        s_instance = nullptr;
    }
}

void WebServer::remove_conn_by_fd(int sockfd)
{
    if (s_instance)
    {
        s_instance->remove_conn(sockfd);
    }
}

http_conn *WebServer::acquire_conn()
{
    m_conn_pool_lock.lock();
    if (m_free_conns.empty())
    {
        m_conn_pool_lock.unlock();
        return nullptr;
    }

    http_conn *conn = m_free_conns.back();
    m_free_conns.pop_back();
    m_conn_pool_lock.unlock();
    return conn;
}

void WebServer::recycle_conn(http_conn *conn)
{
    if (!conn) return;
    m_conn_pool_lock.lock();
    m_free_conns.push_back(conn);
    m_conn_pool_lock.unlock();
}

bool WebServer::handoff_connection(int connfd, const sockaddr_in &address)
{
    http_conn *conn = acquire_conn();
    if (!conn || m_reactors.empty()) {
        if (conn) recycle_conn(conn);
        utils.show_error(connfd, "Internal server busy");
        return false;
    }
    SubReactor *reactor = m_reactors[m_next_reactor++ % m_reactors.size()];
    reactor->enqueue_connection(conn, connfd, address);
    return true;
}

client_data *WebServer::acquire_client_data()
{
    if (m_free_client_data.empty())
    {
        return nullptr;
    }

    client_data *data = m_free_client_data.back();
    m_free_client_data.pop_back();
    data->sockfd = -1;
    data->timer = nullptr;
    return data;
}

util_timer *WebServer::acquire_timer()
{
    if (m_free_timers.empty())
    {
        return nullptr;
    }

    util_timer *timer = m_free_timers.back();
    m_free_timers.pop_back();
    timer->expire = 0;
    timer->cb_func = nullptr;
    timer->user_data = nullptr;
    timer->prev = nullptr;
    timer->next = nullptr;
    return timer;
}

void WebServer::release_client_data(client_data *data)
{
    if (!data)
    {
        return;
    }

    data->sockfd = -1;
    data->timer = nullptr;
    m_free_client_data.push_back(data);
}

void WebServer::release_timer_by_ptr(util_timer *timer)
{
    if (s_instance)
    {
        s_instance->release_timer(timer);
    }
}

void WebServer::enqueue_completion(http_conn *conn, int sockfd, uint64_t generation,
                                   http_conn::PROCESS_RESULT result)
{
    WebServer *server = s_instance;
    if (!server)
        return;

    int owner = conn->owner_reactor();
    if (owner >= 0 && owner < static_cast<int>(server->m_reactors.size()))
        server->m_reactors[owner]->enqueue_completion(conn, sockfd, generation, result);
}

void WebServer::release_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }

    timer->expire = 0;
    timer->cb_func = nullptr;
    timer->user_data = nullptr;
    timer->prev = nullptr;
    timer->next = nullptr;
    m_free_timers.push_back(timer);
}

void WebServer::remove_conn(int sockfd)
{
    auto it = users.find(sockfd);
    if (it != users.end())
    {
        if (it->second->defer_timeout())
            return;
        it->second->close_conn();
        m_free_conns.push_back(it->second);
        users.erase(it);
    }

    auto timer_it = users_timer.find(sockfd);
    if (timer_it != users_timer.end())
    {
        release_client_data(timer_it->second);
        users_timer.erase(timer_it);
    }
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int reactor_num,
                     int close_log, int actor_model,
                     int max_connections)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_reactor_count = reactor_num > 0 ? reactor_num : 1;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    // 多Reactor中socket I/O固定由Sub Reactor负责，Worker只执行Proactor业务阶段。
    (void)actor_model;
    m_actormodel = 0;
    m_max_connections = max_connections > 0 ? max_connections : DEFAULT_MAX_CONNECTIONS;

    m_conn_pool.reserve(m_max_connections);
    m_free_conns.reserve(m_max_connections);
    m_client_data_pool.reserve(m_max_connections);
    m_free_client_data.reserve(m_max_connections);
    m_timer_pool.reserve(m_max_connections);
    m_free_timers.reserve(m_max_connections);
    for (int i = 0; i < m_max_connections; ++i)
    {
        http_conn *conn = new http_conn();
        m_conn_pool.push_back(conn);
        m_free_conns.push_back(conn);

        client_data *data = new client_data();
        data->sockfd = -1;
        data->timer = nullptr;
        m_client_data_pool.push_back(data);
        m_free_client_data.push_back(data);

        util_timer *timer = new util_timer();
        m_timer_pool.push_back(timer);
        m_free_timers.push_back(timer);
    }
}

//初始化日志
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            //异步方式
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            //同步方式
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}


//初始化创建线程池
void WebServer::thread_pool()
{
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);//m_connPool是数据库连接池
}

//初始化创建共享数据库连接池
void WebServer::sql_pool()
{
    m_connPool = connection_pool::GetInstance();//初始化线程连接池单例
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    http_conn init_conn;
    init_conn.initmysql_result(m_connPool);
}


void WebServer::trig_mode()
{
    //注册epoll的触发模式
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
    // std::cout << "m_LISTENTrigmode = " << m_LISTENTrigmode << std::endl;
    // std::cout << "m_CONNTrigmode = " << m_CONNTrigmode << std::endl;
}

//定时器
//1. 有新的客户端连接，创建定时器，初始化http_conn对象并将connfd加入epoll监听
void WebServer::timer(int connfd, sockaddr_in client_address)
{
    //初始化http_conn对象，在初始化函数中将connfd加入epoll监听
    http_conn *conn = acquire_conn();
    if (!conn)
    {
        utils.show_error(connfd, "Internal server busy");
        LOG_ERROR("%s", "http_conn pool exhausted");
        return;
    }
    client_data *data = acquire_client_data();
    util_timer *timer = acquire_timer();
    if (!data || !timer)
    {
        if (data)
            release_client_data(data);
        if (timer)
            release_timer(timer);
        m_free_conns.push_back(conn);
        utils.show_error(connfd, "Internal server busy");
        LOG_ERROR("%s", "timer pool exhausted");
        return;
    }

    conn->init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user,
               m_passWord, m_databaseName, m_epollfd, m_connPool, 0);
    users[connfd] = conn;

    //创建定时器，初始化定时器节点
    //1. 初始化定时器节点中的用户数据结构client_data 
    data->address = client_address;
    data->sockfd = connfd;
    //2. 初始化创建定时器节点
    timer->user_data = data;//节点中的用户信息
    timer->cb_func = cb_func;//回调函数，定时器超时进行删除epoll监听和关闭连接的操作
    time_t cur = time(NULL);//当前时间，用于设置定时器的绝对超时时间
    timer->expire = cur + 3 * TIMESLOT;//定时器的超时时间：连接最多保持3个TIMESLOT时间的非活跃状态
    data->timer = timer;
    users_timer[connfd] = data;
    //3. 将定时器节点加入定时器容器
    utils.m_timer_lst.add_timer(timer);
}
//2. 连接重新活跃（成功进行读/写操作），重新延迟定时器
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(nullptr);
    timer->expire = cur + 3 * TIMESLOT;//重新设置定时器的超时时间
    utils.m_timer_lst.adjust_timer(timer);//调整定时器在链表中的位置（向后调整）,链表是升序的

    LOG_INFO("%s", "adjust timer once");
}
//3. 客户端关闭连接，删除对应的定时器、关闭socketfd和从epoll中移除（cb_func实现）
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    if(timer){
        timer->cb_func(timer->user_data);//cb_func函数删除epoll中非活动连接的客户端socket、关闭连接
        //从utils对象中的定时器容器中删除对应的定时器
        utils.m_timer_lst.del_timer(timer);
    }
    else
    {
        remove_conn(sockfd);
    }

    LOG_INFO("close fd %d", sockfd);
}

//处理服务端收到客户端 连接请求
bool WebServer::dealclientdata(){
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    //listenfd的触发模式默认为LT
    if(m_LISTENTrigmode == 0){
        //LT模式下,只要listenfd有事件发生,就会执行一次accept

        //接受新客户端连接
        int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlength);
        if(connfd < 0){
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }

        //服务器连接数量达到上限了，拒绝浏览器的连接
        if (!handoff_connection(connfd, client_address))
            LOG_ERROR("%s", "Internal server busy");
    }

    else{
        //ET模式下,需要循环接受客户端连接,直到accept返回EAGAIN

        while(1){
            int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlength);
            if(connfd < 0){
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }

            //服务器连接数量达到上限了，拒绝浏览器的连接
            if (!handoff_connection(connfd, client_address))
                LOG_ERROR("%s", "Internal server busy");
        }
        return false;
    }

    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;//信号值:SIGALRM-SIGTERM
    char signals[1024];//信号值的字符串形式,通过管道读端recv函数读取获得
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);//从管道读端读取信号值到signals中

    //ret == -1:读取失败
    //ret == 0:没有信号
    //ret > 0:读取到信号的字节，这里一般只有一个字节，即信号值（SIGALRM-SIGTERM）
    if(ret == -1 || ret == 0) return false;
    else{
        //解析信号值，以便在主循环eventloop中进行对应的处理
        for(int i = 0; i < ret; ++i){
            switch(signals[i]){
            case SIGALRM:{
                //处理定时器超时信号，通知主循环检查定时器链表中是否有到期的定时器
                timeout = true;
                break;
            }
            case SIGTERM:{
                //停止服务器：主循环while(!stop_server)结束，退出循环
                stop_server = true;
                break;
            }
            }
         }
    }
    return true;
}

//处理信号


/*处理客户端fd的读事件(接收数据)
* 事件处理模式可选React模式或Proactor模式
* 并发模式默认是proactor
* 读写事件都表示连接重新活跃了，需要重新设置定时器(adjust_timer)
*/
void WebServer::dealwithread(int sockfd){
    auto timer_it = users_timer.find(sockfd);
    if (timer_it == users_timer.end())
    {
        return;
    }
    util_timer *timer = timer_it->second->timer;
    auto it = users.find(sockfd);
    if (it == users.end())
    {
        return;
    }
    http_conn *conn = it->second;

    //Reactor模式下，直接将fd交给工作线程，由工作线程处理socket读数据操作
    if(m_actormodel == 1){
        if (timer)
        {
            //连接重新活跃，重新设置定时器
            adjust_timer(timer);
        }

        //主线程将读事件放到线程池请求队列中就结束了，其它的交给线程池
        if (!m_pool->append(conn, 0))
        {
            LOG_ERROR("reactor read queue full, close fd %d", sockfd);
            deal_timer(timer, sockfd);
            return;
        }

        //等待事件别工作线程读取完，进入解析状态
        while(true){//由于与工作线程分开的，所以需要while等待工作线程读/写完数据再关闭定时器
            if(conn->improv == 1){//任务被工作线程取出就会置1
                //完成读/写后就关闭定时器
                if (1 == conn->timer_flag)
                {
                    conn->timer_flag = 0;
                    conn->improv = 0;
                    deal_timer(timer, sockfd);
                    break;
                }
                conn->improv = 0;//重置该fd对应的http为0
                break;
            }
        }
    }
    //proactor模式下，主线程先调用http_conn的read_once()读取数据，然后再将存有读取结果的http_conn对象放入线程池
    //也就是工作线程只处理http_conn对象的报文解析处理业务工作，不对socket进行读写
    else{
        if(conn->read_once()){//主线程中先处理读事件
            LOG_INFO("deal with the client(%s)", inet_ntoa(conn->get_address()->sin_addr));

            //将读取的数据放在线程池请求队列中进行解析和打包响应
            conn->begin_processing();
            if (!m_pool->append_p(conn))
            {
                conn->cancel_processing();
                LOG_ERROR("request queue full, close fd %d", sockfd);
                deal_timer(timer, sockfd);
                return;
            }

            //成功接收数据，重新设置定时器表示连接重新活跃
            if (timer)
            {
                //连接重新活跃，重新设置定时器
                adjust_timer(timer);
            }
        }
        else
        {
            //接收数据失败，关闭连接和定时器
            deal_timer(timer, sockfd);
        }
    }
}

/*处理客户端fd的写事件(发送数据)
* 事件处理模式可选React模式或Proactor模式
* 并发模式默认是proactor
* 读写事件都表示连接重新活跃了，需要重新设置定时器(adjust_timer)
*/
void WebServer::dealwithwrite(int sockfd){
    auto timer_it = users_timer.find(sockfd);
    if (timer_it == users_timer.end())
    {
        return;
    }
    util_timer *timer = timer_it->second->timer;
    auto it = users.find(sockfd);
    if (it == users.end())
    {
        return;
    }
    http_conn *conn = it->second;

    auto dispatch_buffered_request = [this, conn, timer, sockfd]() -> bool {
        if (!conn->has_buffered_request())
            return true;

        if (m_actormodel == 0)
            conn->begin_processing();
        bool queued = (m_actormodel == 1) ? m_pool->append(conn, 0) : m_pool->append_p(conn);
        if (!queued)
        {
            if (m_actormodel == 0)
                conn->cancel_processing();
            LOG_ERROR("pipeline request queue full, close fd %d", sockfd);
            deal_timer(timer, sockfd);
            return false;
        }

        if (timer)
            adjust_timer(timer);
        return true;
    };

    //Reactor模式下，直接将fd交给工作线程，由工作线程处理socket写数据操作
    if(m_actormodel == 1){
        if (timer)
        {
            adjust_timer(timer);
        }

        //主线程将写事件放到线程池请求队列中就结束了，其它的交给线程池
        if (!m_pool->append(conn, 1))
        {
            LOG_ERROR("reactor write queue full, close fd %d", sockfd);
            deal_timer(timer, sockfd);
            return;
        }

        while(true){
            if(conn->improv == 1){//任务被工作线程取出就会置1
                if (1 == conn->timer_flag)
                {
                    conn->timer_flag = 0;
                    conn->improv = 0;
                    deal_timer(timer, sockfd);
                    break;
                }
                conn->improv = 0;
                dispatch_buffered_request();
                break;
            }
        }
    }
    //proactor模式下，主线程先调用http_conn的write()发送数据，然后再将存有写结果的http_conn对象放入线程池
    //也就是工作线程只处理http_conn对象的报文解析处理业务工作，不对socket进行读写
    //写事件一般是在响应中打包完数据了，所以写完就结束了，这里不需要再将任务放进线程池中
    else{
        if(conn->write()){//主线程中先处理写事件
            LOG_INFO("send data to the client(%s)", inet_ntoa(conn->get_address()->sin_addr));

            //成功发送数据，重新设置定时器表示连接重新活跃
            if (timer)
            {
                adjust_timer(timer);
            }
            dispatch_buffered_request();
        }
        else
        {
            //发送数据失败，关闭连接和定时器
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithcompletion()
{
    uint64_t value;
    while (read(m_completionfd, &value, sizeof(value)) > 0)
    {
    }

    std::vector<completion_event> completed;
    m_completion_lock.lock();
    completed.swap(m_completions);
    m_completion_lock.unlock();

    for (const completion_event &event : completed)
    {
        auto user_it = users.find(event.sockfd);
        if (user_it == users.end() || user_it->second != event.conn ||
            event.conn->generation() != event.generation)
            continue;

        bool timed_out = event.conn->finish_processing();
        if (timed_out)
        {
            remove_conn(event.sockfd);
            continue;
        }

        if (event.result == http_conn::PROCESS_NEED_READ)
        {
            event.conn->arm_read();
        }
        else if (event.result == http_conn::PROCESS_READY_WRITE)
        {
            event.conn->arm_write();
        }
        else
        {
            auto timer_it = users_timer.find(event.sockfd);
            util_timer *timer = timer_it == users_timer.end() ? nullptr : timer_it->second->timer;
            deal_timer(timer, event.sockfd);
        }
    }
}

void WebServer::eventListen(){
    //socket编程
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);//创建socket
    assert(m_listenfd >= 0);//断言，如果m_listenfd<0，程序终止

    //是否优雅关闭socket连接: 优雅关闭是指等待数据发送完毕再关闭
    //默认为0，即不等待
    //setsocketopt设置打开的socket的属性:SO_LINGER设置关闭socket时的行为
    // struct linger {
    //     int l_onoff;    // 延迟关闭的开关标志
    //     int l_linger;   // 延迟关闭的时间（秒）
    // };
    if(m_OPT_LINGER  == 0){
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }else if(m_OPT_LINGER == 1){
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    //设置socket的IP和端口
    struct sockaddr_in address;
    bzero(&address, sizeof(address));//将内存清0
    address.sin_family = AF_INET;
    address.sin_port = htons(m_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);//监听主机的所有网卡

    //绑定和监听socket
    //SO_REUSEADDR选项开启允许端口重用
    int flags = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(flags));
    //绑定
    int ret = 0;
    ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);
    //监听
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);
    LOG_INFO("%s%d", "listen the port ", m_port);

    // utils.init(TIMESLOT);

    //创建epoll对象
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    //将监听的socket加入epoll监听
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);

    //通过socketpair创建全双工管道,管道也是一种文件描述符
    //管道作用:可以通过管道在程序中实现进程间通信
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);//创建全双工管道读端m_pipefd[0]和写端m_pipefd[1]：写端是定时器向epoll通知事件的，epoll监听读端
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);//设置写端非阻塞
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);//将读端加入主线程epoll监听
    m_completionfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(m_completionfd != -1);
    utils.addfd(m_epollfd, m_completionfd, false, 0);
    http_conn::m_completion_cb = WebServer::enqueue_completion;
    for (int i = 0; i < m_reactor_count; ++i)
    {
        SubReactor *reactor = new SubReactor(this, i);
        if (!reactor->start())
        {
            delete reactor;
            throw std::runtime_error("failed to start sub reactor");
        }
        m_reactors.push_back(reactor);
    }
    //绑定不同信号（SIGPIPE-SIGALRM-SIGTERM）的信号处理函数（忽略 or sig_handler发送sig标识）
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);//启动定时器，每TIMESLOT秒发送SIGALRM信号（整个程序中只有一个真实的定时器，定时器容器中的是存储超时的绝对时间来与这个唯一的timeout处理进行比较）

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
    Utils::u_close_conn_cb = WebServer::remove_conn_by_fd;
    Utils::u_release_timer_cb = WebServer::release_timer_by_ptr;

}

//主循环:epoll_wait阻塞监听事件
void WebServer::eventLoop(){
    bool timeout = false;
    bool stop_server = false;

    while(!stop_server){
        //epoll_wait设置为-1,也就是阻塞监听事件
        //当有事件发生时,epoll_wait返回事件个数number,且事件存在events数组中
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);

        //遍历events数组,处理就绪事件
        if(number < 0 && errno != EINTR){
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        for (int i = 0; i < number; i++){
            int sockfd = events[i].data.fd;

            //listenfd有事件发生:有新的连接
            if(sockfd == m_listenfd){
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            //对方异常断开连接
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //对方异常断开连接,从定时器容器中移除对应的定时器节点
                LOG_ERROR("%s", "EPOLLRDHUP | EPOLLHUP | EPOLLERR");
                auto timer_it = users_timer.find(sockfd);
                if (timer_it == users_timer.end())
                    continue;
                util_timer *timer = timer_it->second->timer;
                deal_timer(timer, sockfd);//主动删除定时器和关闭连接
            }
            //管道读端有事件发生:信号处理，通过dealwithsignal从epoll管道读端读取信号，并解析对应的信号（SIGALRM-SIGTERM）
            else if((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)){
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            else if((sockfd == m_completionfd) && (events[i].events & EPOLLIN)){
                dealwithcompletion();
            }
            //处理客户fd连接上接收到的数据
            else if(events[i].events & EPOLLIN){
                dealwithread(sockfd);//处理读事件
            }
            else if(events[i].events & EPOLLOUT){
                dealwithwrite(sockfd);//处理写事件
            }
        }

        // 处理定时器事件:timer tick定时中断,执行timer_handler处理链表上到期的节点
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
