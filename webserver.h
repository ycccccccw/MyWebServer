#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <cassert>
#include <errno.h>
#include <stdlib.h>
#include <unordered_map>
#include <vector>
#include <stdint.h>

// epoll并发
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>//getopt，close，alarm函数
#include <netinet/in.h>//sockaddr_in
#include <arpa/inet.h>//inet_addr
#include <fcntl.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int DEFAULT_MAX_CONNECTIONS = 1024; //默认最大并发连接数
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位-定时器使用

using namespace std;
class SubReactor;

class WebServer{
public:
    WebServer();
    ~WebServer(); 

    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int reactor_num, int close_log, int max_connections);
    
    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();//socket监听，实现epoll
    void eventLoop();//epoll_wait阻塞监听事件
    bool dealclientdata();//处理客户端连接
    bool dealwithsignal(bool& stop_server);//处理SIGTERM信号
    http_conn *acquire_conn();
    void recycle_conn(http_conn *conn);
    bool handoff_connection(int connfd, const sockaddr_in &address);

    static void enqueue_completion(http_conn *conn, int sockfd, uint64_t generation,
                                   http_conn::PROCESS_RESULT result);


public:
    //基础
    int m_port;
    char *m_root;
    int m_log_write;
    int m_close_log;
    int m_max_connections;

    int m_pipefd[2];    //信号处理函数通过管道通知Main Reactor退出
    int m_epollfd;      //Main Reactor的epoll句柄

    //数据库相关
    connection_pool *m_connPool;//共享数据库连接池
    string m_user;              //登陆数据库用户名
    string m_passWord;          //登陆数据库密码
    string m_databaseName;      //使用数据库名
    int m_sql_num;              //数据库连接池数量

    //线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;
    int m_reactor_count;

    //epoll_event相关
    //epoll_event是<sys/epoll.h>中定义的一个结构体，用于注册事件
    //描述在使用 epoll 监听文件描述符时发生的事件
    //这里将最大事件数设为MAX_EVENT_NUMBER
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    Utils utils;

private:
    static WebServer *s_instance;
    locker m_conn_pool_lock;
    std::vector<http_conn *> m_conn_pool;
    std::vector<http_conn *> m_free_conns;
    std::vector<SubReactor *> m_reactors;
    size_t m_next_reactor;
};

#endif
