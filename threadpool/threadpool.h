#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
// #include <cstdio>//printf
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

//这里的模板类主要指http_conn具体类
template <typename T>
class threadpool{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);//工作线程运行函数
    void run();//线程池的主线程运行函数:保证线程池中的线程一直处于等待任务的状态

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列（Tasks）
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
};

//构造函数：初始化参数 && 创建工作线程
template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) :
    m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL){
    
    if(thread_number <= 0 || max_requests <= 0){
        //线程数和请求队列长度必须大于0
        throw std::exception();
    }

    //创建线程数组
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        //创建线程数组失败
        throw std::exception();
    }

    //创建thread_number个线程，并将它们设置为脱离线程
    for(int i = 0; i < thread_number; ++i){
        //1. 创建线程
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){
            //创建线程失败：释放资源并抛出异常
            delete[] m_threads;
            throw std::exception();
        }
        //2. 设置线程为脱离线程：线程结束后自动释放资源
        if(pthread_detach(m_threads[i])){
            //设置线程为脱离线程失败：释放资源并抛出异常
            delete[] m_threads;
            throw std::exception();
        }
    }
}

//析构函数：释放线程数组资源
template <typename T>
threadpool<T>::~threadpool(){
    //由于线程已经被设置为脱离线程，所以这里不需要释放pthread
    delete[] m_threads;
}

//Sub Reactor完成socket I/O后，将业务处理任务加入队列。
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();

    m_queuestat.post();

    return true;
}

//工作线程运行函数:worker
template <typename T>
void *threadpool<T>::worker(void *arg){
    threadpool *pool = static_cast<threadpool *>(arg);
    pool->run();
    return pool;
}
//线程池的主线程运行函数:保证线程池中的线程一直处于等待任务的状态 && 从请求队列中取出任务并执行之
template <typename T>
void threadpool<T>::run(){
    while(true){
        //等待线程池的信号量，即是否有任务需要处理（阻塞等待）
        m_queuestat.wait();

        //再查看确认是否有任务需要处理，如果没有的话就continue继续while循环
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }

        //有任务则取出任务并处理
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if(!request){//任务为空任务
            continue;
        }

        typename T::PROCESS_RESULT result = request->process_async();
        request->notify_completion(result);
    }
}
#endif
