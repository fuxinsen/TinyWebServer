#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

class connection_pool
{
public:
	MYSQL *GetConnection();				 //获取数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接
	int GetFreeConn();					 //获取连接
	void DestroyPool();					 //销毁所有连接

	// 单例模式：保证一个类仅有一个实例，并提供一个访问它的全局访问点。
	static connection_pool *GetInstance();

	void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn); 
	
	connection_pool();
	~connection_pool();

private:
	unsigned int MaxConn;  //最大连接数
	unsigned int CurConn;  //当前已使用的连接数
	unsigned int FreeConn; //当前空闲的连接数

private:
	locker lock;			//互斥量
	list<MYSQL *> connList; //连接池链表 结点是每个连接
	sem reserve;			//信号量

private:
	string url;			 //主机地址
	string Port;		 //数据库端口号
	string User;		 //登陆数据库用户名
	string PassWord;	 //登陆数据库密码
	string DatabaseName; //使用数据库名
};

//RAII：资源获取即初始化；防止资源泄漏。
class connectionRAII{

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);
	~connectionRAII();
	
private:
	MYSQL *conRAII;	//MYSQL是包含某数据库【一个连接】所有信息的结构体
	connection_pool *poolRAII;
};

#endif
