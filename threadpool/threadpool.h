#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>        //异常处理
#include <pthread.h>        //线程
#include "../lock/locker.h" //锁
#include "../CGImysql/sql_connection_pool.h"   //连接池

template <typename T>
class threadpool
{
public:
    //线程池构造函数：需要绑定连接池、线程数、最大请求数
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request); //加入请求队列的函数

private:
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //指向线程ID的指针数组，里面是m_thread_number个线程ID的指针
    std::list<T *> m_workqueue; //请求队列 使用自带链表
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理 信号量
    bool m_stop;                //是否结束线程
    connection_pool *m_connPool;//数据库连接池
};

//线程池中构造函数实现
template <typename T>
threadpool<T>::threadpool(connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        //新创建的线程ID 默认属性 子线程回调函数 回调函数的参数
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        //设置线程分离：终止时会立即释放，而不用使用join等待回收
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

//线程池中析构函数实现
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads; //删除所有线程ID
    m_stop = true;
}

//线程池中append函数的实现 向线程池的工作队列m_workqueue中加入请求
template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

//工作线程运行的函数，它不断从工作队列中取出任务并执行之
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

//线程池中worker函数的实际代码部分
template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())    //请求队列为空时不必处理了
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();   //取出工作队列中队首请求
        m_workqueue.pop_front();            //出队
        m_queuelocker.unlock();     //队列访问完毕，解锁
        if (!request)   continue;   //防止空请求引起错误
        connectionRAII mysqlcon(&request->mysql, m_connPool);
        
        request->process();
    }
}
#endif
