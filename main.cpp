#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时/定时单位

// #define SYNLOG      //同步写日志：直接写
#define ASYNLOG   //异步写日志：放入阻塞队列另开一个线程写

#define listenfdET //边缘触发非阻塞
// #define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2]; //管道的描述符
static sort_timer_lst timer_lst; //定义定时器链表
static int epollfd = 0;          //epoll事件表的描述符

//信号处理函数：给pipefd[1]发送信号sig（一个整数）
void sig_handler(int sig)
{
    //为保证函数的可重入性（可被别的线程打断而不出错），保留原来的errno
    int save_errno = errno;
    int msg = sig;
    //send在标志位为0时与write一样，但是send只能用在socketfd处于连接状态
    send(pipefd[1], (char *)&msg, 1, 0); //给管道写端发送数据
    errno = save_errno;
}

//设置信号函数：sig信号 信号对应的处理函数 是否开启RESTART（可使被该信号打断的【系统调用】自动恢复）
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa)); //初始化
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick(); //记录日志并检查是否到期
    alarm(TIMESLOT); //重新定时5秒
}

//定时器回调函数：删除非活动连接在socket上的【注册事件】，并关闭此fd连接
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

//向指定socket发送错误消息并关闭
void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if (argc <= 1)
    {
        printf("please use command: ./%s [ip_address] port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);

    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "xiaofu1412.", "mydb", 3306, 8); //最多8个
    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool); //保护代码
    }
    catch (...)//处理所有异常：错误直接返回1 退出服务器程序
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD]; //http连接 一个连接就是一个user
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    // address.sin_addr.s_addr = htonl(INADDR_ANY);
    inet_pton( AF_INET, "172.24.10.12", &address.sin_addr.s_addr);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)); //端口复用：允许创建端口号相同但IP地址不同的lfd
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    epoll_event events[MAX_EVENT_NUMBER]; //用于存储epoll事件表中就绪事件的event数组
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false); //将监听描述符（只有这一个）加入epoll监控，注册读事件、LT、非one_shot模式
    http_conn::m_epollfd = epollfd;

    //使用socketpair建立一对匿名的已经连接的套接字，相当于管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd); //pipefd[0]:读 pipefd[1]:写
    assert(ret != -1);
    setnonblocking(pipefd[1]); //pipefd的写设为非阻塞
    addfd(epollfd, pipefd[0], false); //监听pipefd的读事件

    addsig(SIGALRM, sig_handler, false); //设置闹钟信号
    addsig(SIGTERM, sig_handler, false); //设置终止信号
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD]; //创建尽量多的客户信息数据结构

    bool timeout = false;
    alarm(TIMESLOT); //5s后此进程会收到alarm信号，然后会调用sig_handler给pipefd[1]发数据

    while (!stop_server)
    {
        //主线程调用epoll_wait阻塞等待epollfd上的事件，并将所有就绪的事件复制到events数组中，并返回就绪事件个数
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) //错误且错误不是被信号打断引起的
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address); //users是指针，users[x]是指针偏移x个所指元素大小后的指针，这里面进行监听connfd并注册事件

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer; //一个节点
                timer->user_data = &users_timer[connfd]; //设置好用户数据并指定
                timer->cb_func = cb_func; //指定回调函数
                time_t cur = time(NULL); //获取当前时间
                timer->expire = cur + 3 * TIMESLOT; //设置到期时间为15s
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

//使用ET监听
#ifdef listenfdET
                while (1) //ET使用while是防止出现多个lfd请求来到时只触发一次的情况
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }

            //对应描述符发生了客户端关断、挂断、错误事件时，移除
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) 
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]); //这里进行删除和关闭连接

                if (timer)
                {
                    timer_lst.del_timer(timer); //然后删除定时器链表上的结点
                }
            }

            //若这个描述符是管道，并且可以读取，说明是信号处理函数从pipe[1]发来的，就开始处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) 
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0); //读取信号
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout = true; //已经过5s，置好标志位，后面会处理
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true; //终止信号出现，就停止服务器，退出while大循环
                        }
                        }
                    }
                }
            }

            //除了以上几种情况，肯定是已连接描述符的事件：处理可读事件
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once()) //这里已把内容读取到http连接的缓冲区里
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该用户的连接放入请求队列待处理
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT; //更新到期时间
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else //没有读到HTTP报文，直接把这个连接删了
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            //已连接描述符：处理可写事件
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write()) //可写表示缓冲区已准备好，直接发送
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler(); //检查有没有到期并重新设置ALARM
            timeout = false;
        }
    }
    //服务器关闭后，释放资源
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
