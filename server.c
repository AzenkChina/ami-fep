#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "gather.hpp"

#if defined(WIN32)
#define PIPENAME "\\\\?\\pipe\\echo.sock"
#else
#define PIPENAME "/tmp/echo.sock"
#endif

uv_loop_t *loop;

typedef struct {
	uv_write_t req;
	uv_buf_t buf;
} write_req_t;

void free_write_req(uv_write_t *req) {
	write_req_t *wr = (write_req_t*) req;
	free(wr->buf.base);
	free(wr);
}

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
	buf->base = (char *)malloc(suggested_size);
	buf->len = suggested_size;
}

void echo_write(uv_write_t *req, int status) {
	if (status < 0) {
		fprintf(stderr, "Write error %s\n", uv_err_name(status));
	}
	free_write_req(req);
}

void echo_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
	if (nread > 0) {
		packet_header header;
		
#if defined(WIN32)
		//BUG 这里不清楚为什么接收到的数据报文前边多了16个无效字节
		if(nread < (sizeof(header) + 16)) {
#else
		if(nread < sizeof(header)) {
#endif
			return;
		}

		//打印包头信息
		memset(&header, 0, sizeof(header));
#if defined(WIN32)
		memcpy(&header, (void *)((uint8_t *)(buf->base) + 16), sizeof(header));
#else
		memcpy(&header, buf->base, sizeof(header));
#endif
		fprintf(stderr, "Client: %s ID: %08x FLAG: %02x.", header.name, header.id, header.flag);

		//打印包内容
#if defined(WIN32)
		if(nread > (sizeof(header) + 16)) {
			fprintf(stderr, " Message: ");
			for(int i=0; i<(nread - sizeof(header) - 16); i++) {
				fprintf(stderr, " %02x", ((uint8_t *)(buf->base))[sizeof(header) + 16 + i]);
			}
		}
#else
		if(nread > sizeof(header)) {
			fprintf(stderr, " Message: ");
			for(int i=0; i<(nread - sizeof(header)); i++) {
				fprintf(stderr, " %02x", ((uint8_t *)(buf->base))[sizeof(header) + i]);
			}
		}
#endif
		fprintf(stderr, "\n");

		//如果是透传，则返回确认报文
		if(header.flag == PH_TRANSMIT) {
			write_req_t *req = (write_req_t*) malloc(sizeof(write_req_t));
			req->buf.len = sizeof(header) + strlen("Server received.") + 1;
			req->buf.base = (char *)malloc(req->buf.len);
			if(!req->buf.base) {
				free(buf->base);
				return;
			}

			memcpy(req->buf.base, &header, sizeof(header));
			strcpy(req->buf.base + sizeof(header), "Server received.");
			uv_write((uv_write_t *)req, client, &req->buf, 1, echo_write);
		}
	}
	else if (nread < 0) {
		if (nread != UV_EOF)
			fprintf(stderr, "Read error %s\n", uv_err_name(nread));
		uv_close((uv_handle_t*) client, NULL);
	}

	free(buf->base);
}

void on_new_connection(uv_stream_t *server, int status) {
	if (status == -1) {
		// error!
		return;
	}

	uv_pipe_t *client = (uv_pipe_t*) malloc(sizeof(uv_pipe_t));
	uv_pipe_init(loop, client, 0);

	if (uv_accept(server, (uv_stream_t*)client) == 0) {
		uv_read_start((uv_stream_t*)client, alloc_buffer, echo_read);
	}
	else {
		uv_close((uv_handle_t*) client, NULL);
	}
}

void remove_sock(int sig) {
	uv_fs_t req;

	uv_fs_unlink(loop, &req, PIPENAME, NULL);
	exit(0);
}

/**
  * 等待客户端连接
  * 收到客户端发送来的信息后，打印信息，并原封不动的返回
  *
  */
int main() {
	int r;
	uv_pipe_t server;

	loop = uv_default_loop();

	uv_pipe_init(loop, &server, 0);

	signal(SIGINT, remove_sock);

	if((r = uv_pipe_bind(&server, PIPENAME))) {
		fprintf(stderr, "Bind error %s\n", uv_err_name(r));
		return 1;
	}

	if((r = uv_listen((uv_stream_t*) &server, 128, on_new_connection))) {
		fprintf(stderr, "Listen error %s\n", uv_err_name(r));
		return 2;
	}

	return uv_run(loop, UV_RUN_DEFAULT);
}

