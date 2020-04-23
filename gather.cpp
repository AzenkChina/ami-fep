#include <iostream>
#include <queue>
#include <string>
#include <cstring>
#include <stdint.h>
#include <time.h>
#if defined(WIN32)
#include <Ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include "uv.h"
#include "lua.hpp"
#include "gather.hpp"

using namespace std;

#define DEFAULT_AUTH_PACKET_SIZE		128
#define DEFAULT_BACKLOG				128

typedef struct __uni_configs {
	unsigned int port;
	unsigned int timeout;
	unsigned long max_clients;
	char script_registered[512*1024];
	char script_heartbeat[512*1024];
} uni_configs;

typedef struct __uni_runs {
	unsigned long clients;
	uv_connect_t *connection;
	uv_mutex_t lock;
} uni_runs;

typedef struct __uni_write {
	uv_write_t req;
	uv_buf_t buf;
} uni_write;

typedef struct __uni_client {
	uv_tcp_t handle;
	char name[32];
	unsigned char ip[16];
	unsigned short port;
	time_t timestamp;
} uni_client;

typedef struct __uni_classifier {
	uv_work_t req;
	uni_client *client;
	char *packet;
	unsigned int size;
} uni_classifier;



static uni_configs configs;
static uni_runs runs;
static uv_loop_t *loop;
static queue<uni_client *> gc;



/**
  * @brief  获取内存
  */
static void alloc_buffer(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
	buf->base = (char *)malloc(size);
	buf->len = size;
}



/**
  * @brief  连接已关闭
  */
static void on_after_close(uv_handle_t *handle) {
	free(handle);
}

/**
  * @brief  发送数据完成
  */
static void on_after_write(uv_write_t *req, int status) {
	if (status) {
		fprintf(stderr, "Write error %s\n", uv_strerror(status));
	}

	free(((uni_write *)req)->buf.base);
	free(req);
}



/**
  * @brief  管道写数据完成
  */
static void pipe_after_write(uv_write_t *req, int status) {
	if (status) {
		fprintf(stderr, "Write error %s\n", uv_strerror(status));
	}

	free(((uni_write *)req)->buf.base);
	free(req);
}

/**
  * @brief  管道写数据
  */
static void pipe_write_data(char *name, enum __flags flag, char *buffer, int size) {
	uni_write *req;
	packet_header header;
	int rc;
	if(!runs.connection) {
		return;
	}

	req = (uni_write *)malloc(sizeof(*req));
	if(!req) {
		return;
	}

	req->buf.base = (char *)malloc(sizeof(header) + size);
	if(!req) {
		free(req);
		return;
	}
	req->buf.len = sizeof(header) + size;

	//拷贝头
	memset(&header, 0, sizeof(header));
	header.id = time(NULL);
	strcpy(header.name, name);
	header.flag = (uint8_t)flag;
	memcpy(req->buf.base, &header, sizeof(header));
	//拷贝数据
	if(size > 0) {
		memcpy((void *)(sizeof(header) + ((uint64_t)(req->buf.base))), buffer, size);
	}
	if(rc = uv_write((uv_write_t *)req, runs.connection->handle, &req->buf, 1, pipe_after_write)) {
		fprintf(stderr, "uv_write failed: %s", uv_strerror(rc));
	}
}

/**
  * @brief  管道读数据
  */
static void pipe_on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
	int rc;
	packet_header header;
	uv_stream_t dest;

	//有数据报文待读取
	if(nread > 0) {
		//查询对应客户端并发送数据
		if(nread < sizeof(packet_header)) {
			free(buf->base);
			return;
		}

		memcpy(&header, buf->base, sizeof(packet_header));

		//...使用 header.name 查询客户端信息
		if(0) {
			//返回未查询到对应客户端
			pipe_write_data(header.name, RE_OFFLINE, NULL, 0);
			fprintf(stderr, "Client not in map\n");
			free(buf->base);
			return;
		}
		//判断命令
		if(header.flag == (uint8_t)PH_QUERY) {
			//返回客户端在线
			pipe_write_data(header.name, RE_ONLINE, NULL, 0);
			free(buf->base);
			return;
		}
		else if(header.flag == (uint8_t)PH_REJECT) {
			//返回已强制下线客户端
			pipe_write_data(header.name, RE_OK, NULL, 0);
			free(buf->base);
			return;
		}
		else if(header.flag == (uint8_t)PH_TRANSMIT) {
			//开始发送数据到客户端
			uni_write *wreq = (uni_write *)malloc(sizeof(uni_write));
			if(!wreq) {
				pipe_write_data(header.name, RE_FAILD, NULL, 0);
				free(buf->base);
				return;
			}
			wreq->buf.base = (char *)malloc(nread - sizeof(packet_header));
			if(!(wreq->buf.base)) {
				pipe_write_data(header.name, RE_FAILD, NULL, 0);
				free(wreq);
				free(buf->base);
				return;
			}
			wreq->buf.len = nread - sizeof(packet_header);
			memcpy(wreq->buf.base, buf->base + sizeof(packet_header), nread - sizeof(packet_header));
			if(rc = uv_write((uv_write_t *)wreq, &dest, &wreq->buf, 1, on_after_write)) {
				pipe_write_data(header.name, RE_FAILD, NULL, 0);
				free(wreq);
				free(buf->base);
				fprintf(stderr, "uv_write failed: %s", uv_strerror(rc));
				return;
			}

			//返回数据发送成功
			pipe_write_data(header.name, RE_OK, NULL, 0);
			free(buf->base);
			return;
		}
	}
	else if (nread < 0) {
		if (nread != UV_EOF) {
			fprintf(stderr, "Read pipe error %s\n", uv_err_name(nread));
			uv_close((uv_handle_t *)client, NULL);
		}
	}

	free(buf->base);
}

/**
  * @brief  管道建立
  */
static void on_pipe_connect(uv_connect_t *connect, int status) {
	int rc;
	runs.connection = (uv_connect_t *)0;
	if(status < 0) {
		fprintf(stderr, "Invalid pipe connection.");
	}
	else {
		runs.connection = connect;
		if(rc = uv_read_start(runs.connection->handle, alloc_buffer, pipe_on_read)) {
			fprintf(stderr, "uv_read_start failed: %s", uv_strerror(rc));
		}
	}
}



/**
  * @brief  注册报文判断完成
  */
static void on_after_register(uv_work_t *req, int status) {
	free(((uni_classifier *)req)->packet);
	free(req);
}

/**
  * @brief  注册报文判断
  */
static void on_register(uv_work_t *req) {
	//新建虚拟机实例
	lua_State *L = luaL_newstate();
	if(!L) {
		return;
	}
	//初始化虚拟机
	luaL_openlibs(L);
	//传入报文
	lua_newtable(L);
	lua_pushnumber(L, -1);
	lua_rawseti(L, -2, 0);
	for(int n=0; n<((uni_classifier *)req)->size; n++) {
		lua_pushinteger(L, ((uni_classifier *)req)->packet[n]);
		lua_rawseti(L, -2, n+1);
	}
	lua_setglobal(L, "packet");
	//执行脚本
	luaL_dostring(L, configs.script_registered);
	//获取返回值
	char *result = (char *)lua_tostring(L, -1);
	if(result && (strlen(result) > 0) && (strlen(result) < sizeof(((uni_classifier *)req)->client->name))) {
		//写入客户端名称
		strcpy(((uni_classifier *)req)->client->name, result);
	}
	//关闭虚拟机实例
	lua_close(L);
}

/**
  * @brief  心跳报文判断完成
  */
static void on_after_heartbeat(uv_work_t *req, int status) {
	free(((uni_classifier *)req)->packet);
	free(req);
}

/**
  * @brief  心跳报文判断
  */
static void on_heartbeat(uv_work_t *req) {
	//新建虚拟机实例
	lua_State *L = luaL_newstate();
	if(!L) {
		return;
	}
	//初始化虚拟机
	luaL_openlibs(L);
	//传入报文
	lua_newtable(L);
	lua_pushnumber(L, -1);
	lua_rawseti(L, -2, 0);
	for(int n=0; n<((uni_classifier *)req)->size; n++) {
		lua_pushinteger(L, ((uni_classifier *)req)->packet[n]);
		lua_rawseti(L, -2, n+1);
	}
	lua_setglobal(L, "packet");
	//传入客户端名称
	lua_newtable(L);
	lua_pushnumber(L, -1);
	lua_rawseti(L, -2, 0);
	for(int n=0; n<strlen(((uni_classifier *)req)->client->name); n++) {
		lua_pushinteger(L, ((uni_classifier *)req)->client->name[n]);
		lua_rawseti(L, -2, n+1);
	}
	lua_setglobal(L, "client");
	//执行脚本
	luaL_dostring(L, configs.script_heartbeat);
	//获取返回值
	int result = lua_toboolean(L, -1);
	//返回结果
	if(result) {
		((uni_classifier *)req)->client->timestamp = time(NULL);
	}
	//关闭虚拟机实例
	lua_close(L);
}

/**
  * @brief  读取数据
  */
static void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
	int rc;

	//有数据报文待读取
	if(nread > 0) {
		if((((uni_client *)client)->timestamp < time(NULL)) && \
		((time(NULL) - ((uni_client *)client)->timestamp) > configs.timeout)) {
			//该客户端已经超时，关闭并推送到gc列表
			uv_close((uv_handle_t *)client, NULL);
			int retry = 50;
			while(uv_mutex_trylock(&runs.lock) != 0) {
				if(!retry) {
					free(buf->base);
					return;
				}
				retry -= 1;
#if defined ( WIN32 )
				Sleep(1);
#else
				usleep(1*1000);
#endif
			}
			gc.push((uni_client *)client);
			uv_mutex_unlock(&runs.lock);
			free(buf->base);
			return;
		}

		//判断是否已经注册
		if(!(((uni_client *)client)->name[0])) {
			//判断是否为注册报文
			//长度小于等于 DEFAULT_AUTH_PACKET_SIZE 字节的报文才有可能是注册帧
			if(nread <= DEFAULT_AUTH_PACKET_SIZE) {
				//启动注册流程
				//将注册工作发送到其它线程
				uni_classifier *work_req = (uni_classifier *)malloc(sizeof(*work_req));
				if(!work_req) {
					free(buf->base);
					fprintf(stderr, "No memory for work_req\n");
					return;
				}

				memset(work_req, 0, sizeof(*work_req));
				work_req->client = (uni_client *)client;
				work_req->packet = buf->base;
				work_req->size = nread;
				if((rc = uv_queue_work(loop, (uv_work_t *)work_req, on_register, on_after_register))) {
					free(work_req);
					free(buf->base);
					fprintf(stderr, "uv_queue_work failed: %s", uv_strerror(rc));
					return;
				}
				else {
					return;
				}
			}
		}
		else {
			char name[32];
			strcpy(name, ((uni_client *)client)->name);

			//判断是否为心跳报文
			//长度小于等于 DEFAULT_AUTH_PACKET_SIZE 字节的报文才有可能是心跳帧
			if(nread <= DEFAULT_AUTH_PACKET_SIZE) {
				//启动心跳流程
				//将心跳处理发送到其它线程
				uni_classifier *work_req = (uni_classifier *)malloc(sizeof(*work_req));
				if(!work_req) {
					free(buf->base);
					fprintf(stderr, "No memory for work_req\n");
					return;
				}

				memset(work_req, 0, sizeof(*work_req));
				work_req->client = (uni_client *)client;
				work_req->packet = buf->base;
				work_req->size = nread;
				if((rc = uv_queue_work(loop, (uv_work_t *)work_req, on_heartbeat, on_after_heartbeat))) {
					free(work_req);
					free(buf->base);
					fprintf(stderr, "uv_queue_work failed: %s", uv_strerror(rc));
					return;
				}
				else {
					//报文从管道发送到上层
					pipe_write_data(name, PH_TRANSMIT, buf->base, nread);
					return;
				}
			}
			else {
				//报文从管道发送到上层
				pipe_write_data(name, PH_TRANSMIT, buf->base, nread);
			}
		}
	}
	else if (nread < 0) {
		if (nread != UV_EOF) {
			fprintf(stderr, "Read error %s\n", uv_err_name(nread));
		}
		//该客户端已经出错，关闭并推送到gc列表
		uv_close((uv_handle_t *)client, NULL);
		int retry = 50;
		while(uv_mutex_trylock(&runs.lock) != 0) {
			if(!retry) {
				free(buf->base);
				return;
			}
			retry -= 1;
#if defined ( WIN32 )
			Sleep(1);
#else
			usleep(1*1000);
#endif
		}
		gc.push((uni_client *)client);
		uv_mutex_unlock(&runs.lock);
	}

	free(buf->base);
}



/**
  * @brief  新连接
  */
static void on_new_connection(uv_stream_t *server, int status) {
	int rc;

	if (status < 0) {
        fprintf(stderr, "New connection error %s\n", uv_strerror(status));
		return;
	}

	//生成客户端
	uni_client *client = (uni_client *)malloc(sizeof(uni_client));
	if(!client) {
		fprintf(stderr, "No memory for client\n");
		return;
	}
	//初始化客户端
	if((rc = uv_tcp_init(loop, &client->handle))) {
		free(client);
		fprintf(stderr, "uv_tcp_init failed: %s", uv_strerror(rc));
		return;
	}
	//接受客户端连接
	if(uv_accept(server, (uv_stream_t *)&client->handle) == 0) {
		struct sockaddr peername;
		int namelen;

		//获取对端的信息
		memset(&peername, 0, sizeof(peername));
		namelen = sizeof(peername);
		if((rc = uv_tcp_getpeername((const uv_tcp_t *)&client->handle, &peername, &namelen))) {
			uv_close((uv_handle_t *)client, on_after_close);
			fprintf(stderr, "uv_tcp_getpeername failed: %s", uv_strerror(rc));
			return;
		}

		memcpy((void *)client->ip, (const void *)&(((struct sockaddr_in *)&peername)->sin_addr), sizeof(((struct sockaddr_in *)&peername)->sin_addr));
		client->port = ntohs(((struct sockaddr_in *)&peername)->sin_port);
		client->timestamp = time(NULL);

		//允许读操作
		if((rc = uv_read_start((uv_stream_t *)&client->handle, alloc_buffer, on_read))) {
			uv_close((uv_handle_t *)client, on_after_close);
			fprintf(stderr, "uv_read_start failed: %s", uv_strerror(rc));
			return;
		}
	}
	else {
		//不接受客户端连接
		uv_close((uv_handle_t *)client, on_after_close);
	}
}



/**
  * @brief  定时器回调
  */
static void on_timer_triggered(uv_timer_t *handle) {
	static bool running = false;
	static uint64_t size = 0;

	//防止重入
	if(running == true) {
		return;
	}
	running = true;

	if(size > gc.size()) {
		size = gc.size();
	}

	if(size == 0) {
		size = gc.size();
		running = false;
		return;
	}

	if(uv_mutex_trylock(&runs.lock) != 0) {
		size = gc.size();
		running = false;
		return;
	}

	//回收已经关闭的连接
	for(uint64_t n=0; n<size; n++) {
		free(gc.front());
		gc.pop();
	}

	size = gc.size();
	uv_mutex_unlock(&runs.lock);

	running = false;
}



/**
  * @brief  参数列表 -> 监听端口 上行管道名 超时秒数 注册脚本 心跳脚本
  * .\gather.exe 4056 echo 300 script/register.lua script/heartbeat.lua
  * ./gather 4056 echo 300 script/register.lua script/heartbeat.lua
  */
int main(int argc, char **argv) {
	struct sockaddr_in addr;
	uv_tcp_t server;
	uv_pipe_t client;
	uv_timer_t timer;
	uv_connect_t connection;
	char sock[128];
	FILE *fp;
	int rc;

	memset((void *)&configs, 0, sizeof(configs));
	memset((void *)&runs, 0, sizeof(runs));

	//创建事件轮询
	loop = uv_default_loop();

	//判断参数有效性
	if(argc != 5) {
		fprintf(stderr, "Invalid parameter amount\n");
		return 0;
	}

	//端口
	configs.port = atoi(argv[1]);
	if((configs.port <= 1023) || (configs.port > 65535)) {
		fprintf(stderr, "Invalid parameter : port\n");
		return 1;
	}

	//管道
	if(rc = uv_pipe_init(loop, &client, 0)) {
		fprintf(stderr, "uv_pipe_init failed %s\n", uv_strerror(rc));
		return 1;
	}
	else {
		if((strlen(argv[2]) <= 0) || ((strlen(argv[2]) + 64) > sizeof(sock))) {
			fprintf(stderr, "Invalid parameter : sock\n");
		}

		memset(sock, 0, sizeof(sock));
#if defined(WIN32)
		sprintf(sock, "\\\\?\\pipe\\%s.gather", argv[2]);
#else
		sprintf(sock, "/tmp/%s.gather", argv[2]);
#endif
		uv_pipe_connect(&connection, &client, (const char *)sock, on_pipe_connect);
	}

	//超时时间
	configs.timeout = atoi(argv[3]);
	if((configs.timeout <= 0) || (configs.timeout > 65535)) {
		fprintf(stderr, "Invalid parameter : timeout\n");
		return 1;
	}

	//脚本文件 注册判断
	if((fp = fopen(argv[4], "rb"))) {
		fseek(fp,0,SEEK_END);
		int fl = ftell(fp);
		fseek(fp,0,SEEK_SET);
		if((fl <= 0) || (fl >= sizeof(configs.script_registered))) {
			fclose(fp);
			fprintf(stderr, "Invalid script file\n");
			return 1;
		}
		else {
			if(fread(configs.script_registered, 1, fl, fp) != fl) {
				fclose(fp);
				fprintf(stderr, "Invalid script file\n");
				return 1;
			}
			fclose(fp);
		}
	}
	else {
		fprintf(stderr, "Invalid script file\n");
		return 1;
	}

	//脚本文件 心跳判断
	if((fp = fopen(argv[5], "rb"))) {
		fseek(fp,0,SEEK_END);
		int fl = ftell(fp);
		fseek(fp,0,SEEK_SET);
		if((fl <= 0) || (fl >= sizeof(configs.script_heartbeat))) {
			fclose(fp);
			fprintf(stderr, "Invalid script file\n");
			return 1;
		}
		else {
			if(fread(configs.script_heartbeat, 1, fl, fp) != fl) {
				fclose(fp);
				fprintf(stderr, "Invalid script file\n");
				return 1;
			}
			fclose(fp);
		}
	}
	else {
		fprintf(stderr, "Invalid script file\n");
		return 1;
	}

	//初始化TCP服务
	if(rc = uv_tcp_init(loop, &server)) {
		fprintf(stderr, "uv_tcp_init failed: %s", uv_strerror(rc));
		return 1;
	}

	//初始化TIMER服务
	if(rc = uv_timer_init(loop, &timer)) {
		fprintf(stderr, "uv_timer_init failed: %s", uv_strerror(rc));
		return 1;
	}

	//设置监听IP和PORT
	if(rc = uv_ip4_addr("0.0.0.0", configs.port, &addr)) {
		fprintf(stderr, "uv_ip4_addr failed: %s", uv_strerror(rc));
		return 1;
	}
	if(rc = uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0)) {
		fprintf(stderr, "uv_tcp_bind failed: %s", uv_strerror(rc));
		return 1;
	}

	//开始监听
	if(rc = uv_listen((uv_stream_t *)&server, DEFAULT_BACKLOG, on_new_connection)) {
		fprintf(stderr, "uv_listen failed %s\n", uv_strerror(rc));
		return 1;
	}

	//启动TIMER服务
	if(rc = uv_timer_start(&timer, on_timer_triggered, 30*1000, (30*1000-100))) {
		fprintf(stderr, "uv_timer_start failed %s\n", uv_strerror(rc));
		return 1;
	}

	//初始化互斥量
	if(rc = uv_mutex_init(&runs.lock)) {
		fprintf(stderr, "uv_mutex_init failed %s\n", uv_strerror(rc));
		return 1;
	}

	//开始事件轮询
	if((rc = uv_run(loop, UV_RUN_DEFAULT))) {
		uv_mutex_destroy(&runs.lock);
		fprintf(stderr, "uv_run failed %s\n", uv_strerror(rc));
		return 1;
	}

	uv_mutex_destroy(&runs.lock);

	return 0;
}
