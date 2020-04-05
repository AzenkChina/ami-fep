#include <iostream>
#include <map>
#include <string>
#include <cstring>
#include <stdint.h>
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

#define DEFAULT_MAX_CLIENTS			(10*10000)
#define DEFAULT_AUTH_PACKET_SIZE		128
#define DEFAULT_BACKLOG				128

typedef struct __uni_configs {
	unsigned int port;
	unsigned int timeout;
	unsigned long max_clients;
	char script_registered[512*1024];
	char script_heartbeat[512*1024];
	char script_traverse[512*1024];
	char script_linked[512*1024];
} uni_configs;

typedef struct __uni_runs {
	uv_mutex_t lock;
	unsigned short timing;
	unsigned long clients;
	uv_connect_t *connection;
} uni_runs;

typedef struct __uni_id {
	uv_stream_t *client;

	bool operator < (const __uni_id &other) const {
		if((unsigned long long)client < (unsigned long long)(other.client)) {
			return true;
		}
		else {
			return false;
		}
	}
} uni_id;

typedef struct __uni_value {
	char name[32];
	unsigned char ip[16];
	unsigned short port;
	unsigned short gc;
	unsigned long timer;
} uni_value;

typedef struct __uni_write {
	uv_write_t req;
	uv_buf_t buf;
} uni_write;

typedef struct __uni_new_client {
	uv_work_t req;
	uni_id id;
} uni_new_client;

typedef struct __uni_register {
	uv_work_t req;
	map<uni_id, uni_value>::iterator it;
	char *packet;
	unsigned int size;
} uni_register;





static uni_configs configs;
static uni_runs runs;
static uv_loop_t *loop;
static map<uni_id, uni_value> clients;







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
static void pipe_write_data(map<uni_id, uni_value>::iterator client, enum __flags flag, char *buffer, int size) {
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
	header.id = (uint64_t)client->first.client;
	strcpy(header.name, client->second.name);
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
	uni_id id;

	//有数据报文待读取
	if(nread > 0) {
		//查询对应客户端并发送数据
		if(nread < sizeof(packet_header)) {
			free(buf->base);
			return;
		}

		memcpy(&header, buf->base, sizeof(packet_header));
		id.client = (uv_stream_t *)header.id;

		map<uni_id, uni_value>::iterator it = clients.find(id);
		if(it == clients.end()) {
			//返回未查询到对应客户端
			pipe_write_data(it, RE_OFFLINE, NULL, 0);
			fprintf(stderr, "Client not in map\n");
			free(buf->base);
			return;
		}
		//判断是否已经超时
		if(!it->second.timer) {
			//返回未查询到对应客户端
			pipe_write_data(it, RE_OFFLINE, NULL, 0);
			free(buf->base);
			return;
		}
		//判断名字是否一致
		if(strcmp(it->second.name, header.name) != 0) {
			//返回未查询到对应客户端
			pipe_write_data(it, RE_OFFLINE, NULL, 0);
			free(buf->base);
			return;
		}
		//判断命令
		if(header.flag == (uint8_t)PH_QUERY) {
			//返回客户端在线
			pipe_write_data(it, RE_ONLINE, NULL, 0);
			free(buf->base);
			return;
		}
		else if(header.flag == (uint8_t)PH_REJECT) {
			//返回已强制下线客户端
			pipe_write_data(it, RE_OK, NULL, 0);
			free(buf->base);
			return;
		}
		else if(header.flag == (uint8_t)PH_TRANSMIT) {
			//开始发送数据到客户端
			uni_write *wreq = (uni_write *)malloc(sizeof(uni_write));
			if(!wreq) {
				pipe_write_data(it, RE_FAILD, NULL, 0);
				free(buf->base);
				return;
			}
			wreq->buf.base = (char *)malloc(nread - sizeof(packet_header));
			if(!(wreq->buf.base)) {
				pipe_write_data(it, RE_FAILD, NULL, 0);
				free(wreq);
				free(buf->base);
				return;
			}
			wreq->buf.len = nread - sizeof(packet_header);
			memcpy(wreq->buf.base, buf->base + sizeof(packet_header), nread - sizeof(packet_header));
			if(rc = uv_write((uv_write_t *)wreq, it->first.client, &wreq->buf, 1, on_after_write)) {
				pipe_write_data(it, RE_FAILD, NULL, 0);
				free(wreq);
				free(buf->base);
				fprintf(stderr, "uv_write failed: %s", uv_strerror(rc));
				return;
			}

			//重置定时器
			it->second.timer = configs.timeout;

			//返回数据发送成功
			pipe_write_data(it, RE_OK, NULL, 0);
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
	free(((uni_register *)req)->packet);
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
	for(int n=0; n<((uni_register *)req)->size; n++) {
		lua_pushinteger(L, ((uni_register *)req)->packet[n]);
		lua_rawseti(L, -2, n+1);
	}
	lua_setglobal(L, "packet");
	//执行脚本
	luaL_dostring(L, configs.script_registered);
	//获取返回值
	char *result = (char *)lua_tostring(L, -1);
	if(result && (strlen(result) > 0) && (strlen(result) < sizeof(((uni_register *)req)->it->second.name))) {
		//写入客户端名称
		strcpy(((uni_register *)req)->it->second.name, result);
	}
	//关闭虚拟机实例
	lua_close(L);
}

/**
  * @brief  心跳报文判断
  */
static bool is_heartbeat(uni_register *req) {
	//新建虚拟机实例
	lua_State *L = luaL_newstate();
	if(!L) {
		return(false);
	}
	//初始化虚拟机
	luaL_openlibs(L);
	//传入报文
	lua_newtable(L);
	lua_pushnumber(L, -1);
	lua_rawseti(L, -2, 0);
	for(int n=0; n<((uni_register *)req)->size; n++) {
		lua_pushinteger(L, ((uni_register *)req)->packet[n]);
		lua_rawseti(L, -2, n+1);
	}
	lua_setglobal(L, "packet");
	//传入客户端名称
	lua_newtable(L);
	lua_pushnumber(L, -1);
	lua_rawseti(L, -2, 0);
	for(int n=0; n<strlen(((uni_register *)req)->it->second.name); n++) {
		lua_pushinteger(L, ((uni_register *)req)->it->second.name[n]);
		lua_rawseti(L, -2, n+1);
	}
	lua_setglobal(L, "client");
	//执行脚本
	luaL_dostring(L, configs.script_heartbeat);
	//获取返回值
	int result = lua_toboolean(L, -1);
	//关闭虚拟机实例
	lua_close(L);
	//返回结果
	if(result) {
		return(true);
	}
	else {
		return(false);
	}
}

/**
  * @brief  读取数据
  */
static void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
	int rc;
	//有数据报文待读取
	if(nread > 0) {
		uni_id id;

		id.client = (uv_stream_t *)client;
		//判断客户端是否在map中
		map<uni_id, uni_value>::iterator it = clients.find(id);
		if(it == clients.end()) {
			free(buf->base);
			fprintf(stderr, "Client not in map\n");
			return;
		}
		//判断是否已经超时
		if(!it->second.timer) {
			free(buf->base);
			fprintf(stderr, "Client is dropped\n");
			return;
		}
		//判断是否已经注册
		if(!(it->second.name[0])) {
			//启动注册流程
			//将注册工作发送到其它线程
			uni_register *work_req = (uni_register *)malloc(sizeof(*work_req));
			if(!work_req) {
				free(buf->base);
				fprintf(stderr, "No memory for work_req\n");
				return;
			}

			memset(work_req, 0, sizeof(*work_req));
			//BUG 此处传递指针到work线程，在work线程下此指针是否有效存在不确定性
			work_req->it = it;
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
		else {
			//判断是否为心跳报文
			//长度小于等于 DEFAULT_AUTH_PACKET_SIZE 字节的报文才有可能是心跳帧
			if(nread <= DEFAULT_AUTH_PACKET_SIZE) {
				uni_register work_req;

				memset(&work_req, 0, sizeof(work_req));
				work_req.it = it;
				work_req.packet = buf->base;
				work_req.size = nread;

				//是心跳报文
				if(is_heartbeat(&work_req)) {
					//重置定时器
					it->second.timer = configs.timeout;
				}
				else {
					//报文从管道发送到上层
					pipe_write_data(it, PH_TRANSMIT, buf->base, nread);
				}
			}
			else {
				//报文从管道发送到上层
				pipe_write_data(it, PH_TRANSMIT, buf->base, nread);
			}
		}
	}
	else if (nread < 0) {
		uni_id id;

		if (nread != UV_EOF) {
			fprintf(stderr, "Read error %s\n", uv_err_name(nread));
		}

		id.client = (uv_stream_t *)client;
		if(uv_mutex_trylock(&runs.lock) == 0) {
			//判断客户端是否在map中
			map<uni_id, uni_value>::iterator it = clients.find(id);
			if(it != clients.end()) {
				clients.erase(id);
				uv_close((uv_handle_t *)client, on_after_close);

			}

			uv_mutex_unlock(&runs.lock);
		}

	}

	free(buf->base);
}



/**
  * @brief  新客户端上线操作完成
  */
static void on_after_new_client(uv_work_t *req, int status) {
	free(req);
}

/**
  * @brief  新客户端上线后的操作
  */
static void on_new_client(uv_work_t *req) {
	//新建虚拟机实例
	int rc;
	lua_State *L = luaL_newstate();
	if(!L) {
		return;
	}
	//初始化虚拟机
	luaL_openlibs(L);
	//执行脚本
	luaL_dostring(L, configs.script_linked);
	//获取返回值
	char *result = (char *)lua_tostring(L, -1);
	if(result && (strlen(result) > 0)) {
		//返回值发送到对应客户端
		uni_write *wreq = (uni_write *)malloc(sizeof(uni_write));
		if(!wreq) {
			lua_close(L);
			return;
		}
		wreq->buf.base = (char *)malloc(strlen(result) + 1);
		if(!(wreq->buf.base)) {
			free(wreq);
			lua_close(L);
			return;
		}
		wreq->buf.len = strlen(result) + 1;
		strcpy(wreq->buf.base, result);
		if(rc = uv_write((uv_write_t *)wreq, ((uni_new_client *)req)->id.client, &wreq->buf, 1, on_after_write)) {
			free(wreq);
			lua_close(L);
			fprintf(stderr, "uv_write failed: %s", uv_strerror(rc));
			return;
		}
	}
	//关闭虚拟机实例
	lua_close(L);
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

	//判断客户端是否已达上限
	if(runs.clients >= configs.max_clients) {
		fprintf(stderr, "Client full\n");
		return;
	}

	//生成客户端
	uv_tcp_t *client = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
	if(!client) {
		fprintf(stderr, "No memory for client\n");
		return;
	}
	//初始化客户端
	if((rc = uv_tcp_init(loop, client))) {
		free(client);
		fprintf(stderr, "uv_tcp_init failed: %s", uv_strerror(rc));
		return;
	}
	//接受客户端连接
	if(uv_accept(server, (uv_stream_t *)client) == 0) {
		struct sockaddr peername;
		int namelen;
		uni_id id;
		uni_value value;

		//获取对端的信息
		memset(&peername, 0, sizeof(peername));
		namelen = sizeof(peername);
		if((rc = uv_tcp_getpeername((const uv_tcp_t *)client, &peername, &namelen))) {
			uv_close((uv_handle_t *)client, on_after_close);
			fprintf(stderr, "uv_tcp_getpeername failed: %s", uv_strerror(rc));
			return;
		}
		//生成map成员
		memset(&id, 0, sizeof(id));
		memset(&value, 0, sizeof(value));
		id.client = (uv_stream_t *)client;
		memcpy((void *)value.ip, (const void *)&(((struct sockaddr_in *)&peername)->sin_addr), sizeof(((struct sockaddr_in *)&peername)->sin_addr));
		value.port = ntohs(((struct sockaddr_in *)&peername)->sin_port);
		value.timer = configs.timeout;

		//尝试获取锁
		int retry = 10;
		while(uv_mutex_trylock(&runs.lock) != 0) {
			if(!retry) {
				//拒绝本次连接
				uv_close((uv_handle_t *)client, on_after_close);
				fprintf(stderr, "on_new_connection failed to get lock");
				return;
			}

			retry -= 1;

#if defined ( WIN32 )
			Sleep(10);
#else
			usleep(10*1000);
#endif
		}

		//将客户端加入map中
		clients.insert(pair<uni_id, uni_value>(id, value));
		//允许读操作
		if((rc = uv_read_start((uv_stream_t *)client, alloc_buffer, on_read))) {
			clients.erase(id);
			uv_close((uv_handle_t *)client, on_after_close);
			uv_mutex_unlock(&runs.lock);
			fprintf(stderr, "uv_read_start failed: %s", uv_strerror(rc));
			return;
		}
		uv_mutex_unlock(&runs.lock);

		//新客户端连接后需要的操作
		if(configs.script_linked[0]) {
			uni_new_client *work_req = (uni_new_client *)malloc(sizeof(*work_req));
			if(!work_req) {
				fprintf(stderr, "No memory for work_req\n");
				return;
			}
			//BUG 此处传递id结构体到work线程，在work线程下id结构体内的client指针是否有效存在不确定性
			work_req->id = id;
			if((rc = uv_queue_work(loop, (uv_work_t *)work_req, on_new_client, on_after_new_client))) {
				free(work_req);
				fprintf(stderr, "uv_queue_work failed: %s", uv_strerror(rc));
				return;
			}
		}
	}
	else {
		//不接受客户端连接
		uv_close((uv_handle_t *)client, on_after_close);
	}
}



/**
  * @brief  遍历在线客户端完成
  */
static void on_after_traverse(uv_work_t *req, int status) {
	free(req);
}

/**
  * @brief  遍历在线客户端，强制下线超时的客户端
  */
static void on_traverse(uv_work_t *req) {
	map<uni_id, uni_value> traverse;
	lua_State *L = NULL;

	runs.timing = 0xff;

	//判断脚本有效性
	if(configs.script_traverse[0]) {
		//新建虚拟机实例
		L = luaL_newstate();
		if(L) {
			//初始化虚拟机
			luaL_openlibs(L);

			//执行脚本
			//压入客户端名称
			lua_pushstring(L, "all");
			lua_setglobal(L, "name");
			//压入客户端标识
			lua_pushstring(L, "");
			lua_setglobal(L, "id");
		}
	}

	//获取锁
	int retry = 10;
	while(uv_mutex_trylock(&runs.lock) != 0) {
		if(!retry) {
			if(L) {
				lua_close(L);
			}
			fprintf(stderr, "on_traverse failed to get lock");
			return;
		}

		retry -= 1;

#if defined ( WIN32 )
		Sleep(10);
#else
		usleep(10*1000);
#endif
	}

	if(L) {
		//执行脚本
		luaL_dostring(L, configs.script_traverse);
	}

	//关闭超时的连接
	for(map<uni_id, uni_value>::iterator it = clients.begin(); it != clients.end(); it++) {
		if(it->second.timer) {
			it->second.timer -= 1;
			traverse.insert(pair<uni_id, uni_value>(it->first, it->second));

			if(L) {
				//没有客户端名称不处理
				if(!it->second.name[0]) {
					continue;
				}
				//清空lua栈
				lua_settop(L, 0);
				//压入客户端名称
				lua_pushstring(L, it->second.name);
				lua_setglobal(L, "name");
				//压入客户端标识
				lua_pushfstring(L, "%llu", (unsigned long long)it->first.client);
				lua_setglobal(L, "id");
				//执行脚本
				luaL_dostring(L, configs.script_traverse);
			}
		}
		else {
			uv_close((uv_handle_t *)it->first.client, on_after_close);
		}
	}

	clients.clear();
	swap(clients, traverse);

	uv_mutex_unlock(&runs.lock);

	if(L) {
		//关闭虚拟机实例
		lua_close(L);
	}

	traverse.clear();

	//计算当前客户端数量
	runs.clients = clients.size();

	runs.timing = 0;
}

/**
  * @brief  定时器回调
  */
static void on_timer_triggered(uv_timer_t *handle) {
	int rc;

	//上次的任务未完成，不再执行新任务
	if(runs.timing) {
		fprintf(stderr, "Last task not end\n");
		return;
	}

	//将轮询工作发送到其它线程
	uv_work_t *work_req = (uv_work_t *)malloc(sizeof(*work_req));
	if(!work_req) {
		fprintf(stderr, "No memory for work_req\n");
		return;
	}
	if((rc = uv_queue_work(loop, (uv_work_t *)work_req, on_traverse, on_after_traverse))) {
		free(work_req);
		fprintf(stderr, "uv_queue_work failed: %s", uv_strerror(rc));
		return;
	}
}



/**
  * @brief  参数列表 -> 监听端口 上行管道名 超时分钟数 最大客户端数量 注册脚本 心跳脚本 遍历脚本 连接脚本
  * .\gather.exe 4056 echo 5 10000 script/register.lua script/heartbeat.lua script/traverse.lua script/connect.lua
  * ./gather 4056 echo 5 10000 script/register.lua script/heartbeat.lua script/traverse.lua script/connect.lua
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
	if(argc < 7) {
		printf("Six args need : port timeout max_clients script_registered script_heartbeat script_linked\n");
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

	//最大客户端数量
	configs.max_clients = atoi(argv[4]);
	if((configs.max_clients <= 0) || (configs.max_clients > DEFAULT_MAX_CLIENTS)) {
		fprintf(stderr, "Invalid parameter : max_clients\n");
		return 1;
	}

	//脚本文件 注册判断
	if((fp = fopen(argv[5], "rb"))) {
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
	if((fp = fopen(argv[6], "rb"))) {
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

	//脚本文件 客户端遍历
	if(argc >= 8) {
		if((fp = fopen(argv[7], "rb"))) {
			fseek(fp,0,SEEK_END);
			int fl = ftell(fp);
			fseek(fp,0,SEEK_SET);
			if((fl <= 0) || (fl >= sizeof(configs.script_traverse))) {
				fclose(fp);
				fprintf(stderr, "Invalid script file\n");
				return 1;
			}
			else {
				if(fread(configs.script_traverse, 1, fl, fp) != fl) {
					fclose(fp);
					fprintf(stderr, "Invalid script file\n");
					return 1;
				}
				fclose(fp);
			}
		}
	}

	//脚本文件 连接判断
	if(argc >= 9) {
		if((fp = fopen(argv[8], "rb"))) {
			fseek(fp,0,SEEK_END);
			int fl = ftell(fp);
			fseek(fp,0,SEEK_SET);
			if((fl <= 0) || (fl >= sizeof(configs.script_linked))) {
				fclose(fp);
				fprintf(stderr, "Invalid script file\n");
				return 1;
			}
			else {
				if(fread(configs.script_linked, 1, fl, fp) != fl) {
					fclose(fp);
					fprintf(stderr, "Invalid script file\n");
					return 1;
				}
				fclose(fp);
			}
		}
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
	if(rc = uv_timer_start(&timer, on_timer_triggered, 30*1000, (60*1000-1))) {
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
