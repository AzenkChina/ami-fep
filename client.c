#include <iostream>
#include <map>
#include <string>
#include <cstring>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(WIN32)
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#endif

using namespace std;

#if defined(__linux)
#define SOCKET			int
#define SOCKADDR		struct sockaddr
#define SOCKADDR_IN		struct sockaddr_in
#define INVALID_SOCKET		0
#endif

static uint32_t address = 0;
static uint16_t port = 0;

#if defined ( WIN32 )
static DWORD CALLBACK ThreadTest(PVOID arg)
#else
static void *ThreadTest(void *arg)
#endif
{
#if defined ( _WIN32 )
	WSADATA wsaData;
#endif
	SOCKET sock;
	SOCKADDR_IN addr;

#if defined ( _WIN32 )
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

#if defined ( WIN32 )
	Sleep(1000);
#else
	sleep(1);
#endif

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(sock < 0) {
		fprintf(stderr, "Socket error.\n");
		return(0);
	}

	addr.sin_family = AF_INET;
#if defined ( _WIN32 )
	addr.sin_addr.S_un.S_addr = address;
#elif defined ( __linux )
	addr.sin_addr.s_addr = address;
#endif
	addr.sin_port = htons(port);

	if(connect(sock, (SOCKADDR*)&addr, sizeof(SOCKADDR)) < 0) {
#if defined ( _WIN32 )
		closesocket(sock);
		WSACleanup();
#else
		close(sock);
#endif
		fprintf(stderr, "Client: %llu connect error.\n", (unsigned long long)arg);
		return(0);
	}

	char  message[256];
	int   i;

	memset(message, 0, sizeof(message));
	i  = sprintf(message, "Client: %llu", (unsigned long long)arg);

	if(send(sock, message, strlen(message) + 1, 0) != (strlen(message) + 1)) {
#if defined ( _WIN32 )
		closesocket(sock);
		WSACleanup();
#else
		close(sock);
#endif
		fprintf(stderr, "Client: %llu send error.\n", (unsigned long long)arg);
		return(0);
	}
#if defined ( WIN32 )
	Sleep(1000);
#else
	sleep(1);
#endif

	while(1)
	{
		memset(message, 0, sizeof(message));
		i  = sprintf(message, "Client: %d ", (unsigned long long)arg);
		i += sprintf(message + i, "This is my message.\n");

		if(send(sock, message, strlen(message) + 1, 0) != (strlen(message) + 1)) {
#if defined ( _WIN32 )
			closesocket(sock);
			WSACleanup();
#else
			close(sock);
#endif
			fprintf(stderr, "Client: %llu send error.\n", (unsigned long long)arg);
			return(0);
		}

#if defined ( WIN32 )
		Sleep(1000 + rand() % 4000);
#else
		usleep((1000 + rand() % 4000) * 1000);
#endif
	}

#if defined ( _WIN32 )
	closesocket(sock);
	WSACleanup();
#else
	close(sock);
#endif

	return(0);
}


/**
  * @brief  
  */
int main(int argc, char **argv) {
#if defined ( WIN32 )
	HANDLE hThread;
#else
	pthread_t thread;
	pthread_attr_t thread_attr;
#endif
	
	//判断参数有效性
	if(argc != 4) {
		printf("Three args need : address port clients\n");
		return 0;
	}
	
	//主站地址
	address = inet_addr(argv[1]);
	if(address == INADDR_NONE) {
		printf("Address invalid.\n");
		return 0;
	}
	
	//端口
	port = atol(argv[2]);
	if((port < 0) || (port > 65535)) {
		printf("Port invalid.\n");
		return 0;
	}
	
	//客户端数量
	int clients = atoi(argv[3]);
	if((clients < 1) || (clients > 2048)) {
		printf("Amount invalid.\n");
		return 0;
	}
	
	for(int i=0; i<clients; i++) {
#if defined ( WIN32 )
		hThread = CreateThread(NULL, 0, ThreadTest, (LPVOID)i, 0, NULL);
		CloseHandle(hThread);
		Sleep(10);
#else
		pthread_attr_init(&thread_attr);
		pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
		pthread_create(&thread, &thread_attr, ThreadTest, (void *)i);
		pthread_attr_destroy(&thread_attr);
		usleep(10*1000);
#endif
	}

	getchar();

	return 0;
}

