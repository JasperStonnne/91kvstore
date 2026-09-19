
//server.h
#ifndef __SERVER_H__
#define __SERVER_H__
#include <stddef.h>
#define BUFFER_LENGTH		1024
#define CONNECTION_SIZE (1024*1024)//添加最大链接数量

typedef int (*msg_handler)(
	char *msg,
	int length,
	char *response,
	int response_capacity,
	int *consumed_length
);//通用消息处理函数类型，网络层收到数据后调用

typedef struct {//通用监听器配置，一个监听端口绑定一个消息处理函数
	unsigned short port;
	msg_handler handler;
}kvs_listener_config_t;

int reactor_start(unsigned short port,msg_handler handler);
int ntyco_start(unsigned short port,msg_handler handler);
int ntyco_start_listeners(const kvs_listener_config_t *listeners,size_t listener_count);
int proactor_start(unsigned short port,msg_handler handler);


#define ENABLE_HTTP 0
#define ENABLE_WEBSOCKET 0
#define ENABLE_KVSTORE 1

typedef int (*RCALLBACK)(int fd);
/* 通用输入缓冲区 */
typedef struct kvs_input_buffer{
	char data[BUFFER_LENGTH];
	int length;
}kvs_input_buffer_t;
int kvs_input_buffer_consume(kvs_input_buffer_t *buffer,int consumed_length);

typedef struct kvs_output_buffer{
	char data[BUFFER_LENGTH];
	int length;
	int offset;
}kvs_output_buffer_t;

/*链接状态*/
struct conn {
	int fd;

	kvs_input_buffer_t input;

	kvs_output_buffer_t output;

	RCALLBACK send_callback;

	union {
		RCALLBACK recv_callback;
		RCALLBACK accept_callback;
	} r_action;

	int status;
#if 1 // websocket
	char *payload;
	char mask[4];
#endif
};

#if ENABLE_HTTP
int http_request(struct conn *c);
int http_response(struct conn *c);
#endif

#if ENABLE_WEBSOCKET
int ws_request(struct conn *c);
int ws_response(struct conn *c);
#endif

#if ENABLE_KVSTORE
int kvs_request(struct conn *c);
int kvs_response(struct conn*c);
#endif


#endif

