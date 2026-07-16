#include "webserver.h"
#include "sub_reactor.h"

WebServer *WebServer::s_instance = nullptr;

WebServer::WebServer(){
    s_instance = this;
    m_max_connections = DEFAULT_MAX_CONNECTIONS;
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
    for (http_conn *conn : m_conn_pool)
    {
        delete conn;
    }
    m_conn_pool.clear();
    m_free_conns.clear();
    delete m_pool;
    if (s_instance == this)
    {
        s_instance = nullptr;
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

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int reactor_num,
                     int close_log, int max_connections)
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
    m_max_connections = max_connections > 0 ? max_connections : DEFAULT_MAX_CONNECTIONS;

    m_conn_pool.reserve(m_max_connections);
    m_free_conns.reserve(m_max_connections);
    for (int i = 0; i < m_max_connections; ++i)
    {
        http_conn *conn = new http_conn();
        m_conn_pool.push_back(conn);
        m_free_conns.push_back(conn);
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
    m_pool = new threadpool<http_conn>(m_thread_num);
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

bool WebServer::dealwithsignal(bool &stop_server)
{
    char signals[1024];
    int count = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (count <= 0) return false;
    for (int i = 0; i < count; ++i)
        if (signals[i] == SIGTERM) stop_server = true;
    return true;
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

    //信号处理函数通过自管道安全地通知Main Reactor退出。
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);
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
    //Main Reactor只处理退出信号，连接超时由各Sub Reactor负责。
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    Utils::u_pipefd = m_pipefd;
}

//主循环:epoll_wait阻塞监听事件
void WebServer::eventLoop(){
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
            //信号管道只负责通知Main Reactor退出。
            else if((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)){
                bool flag = dealwithsignal(stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "signal pipe read failure");
            }
        }
    }
}
