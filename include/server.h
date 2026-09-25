
//server.h
#ifndef __SERVER_H__
#define __SERVER_H__
#include <stddef.h>
#define BUFFER_LENGTH		1024
#define CONNECTION_SIZE (1024*1024)//添加最大链接数量

typedef int (*msg_handler)(
	int connection_fd,
	char *msg,
	int length,
	char *response,
	int response_capacity,
	int *consumed_length
);//通用消息处理函数类型，网络层收到数据后调用

typedef int (*kvs_connection_open_handler)(
    int connection_fd,        // 网络层建立成功的 socket
    char *output,             // 首次发送内容写入这个缓冲区
    int output_capacity       // output 最多可以写多少字节
);

typedef void (*kvs_connection_close_handler)(
    int connection_fd         // 已经断开的 socket
);

typedef int (*kvs_connection_stream_handler)(
	int connection_fd,//当前链接 fd
	char *output,//下一块数据写入这里
	int output_capacity//本次最多写入多少字节
);
/*返回值
>0 产生的字节数
=0 数据流已经结束
<0 生成数据失败
*/


typedef struct {
    const char *host;                              // 要连接的目标地址
    unsigned short port;                           // 要连接的目标端口
    kvs_connection_open_handler open_handler;      // 连接成功后调用
    msg_handler message_handler;                   // 收到对方数据后调用
    kvs_connection_close_handler close_handler;    // 连接断开后调用
} kvs_connector_config_t;

typedef int (*kvs_frame_handler)(//单命令处理函数类型
	int connection_fd,
	char *frame,
	int frame_length,
	char *response,
	int response_capacity

);

int kvs_line_batch_protocol(//公共批处理器
    int connection_fd,                 // 当前连接
    char *msg,                          // 整块 TCP 输入数据
    int length,                         // 输入数据长度
    char *response,                     // 响应缓冲区
    int response_capacity,              // 响应缓冲区大小
    int *consumed_length,               // 本次一共消费多少输入
    kvs_frame_handler frame_handler     // 每拆出一条命令后调用谁
);

typedef struct {//通用监听器配置，一个监听端口绑定一个消息处理函数
	unsigned short port;
	msg_handler handler;
	kvs_connection_stream_handler stream_handler; // 可选：连接响应后继续分块输出数据
}kvs_listener_config_t;

int reactor_start(unsigned short port,msg_handler handler);
int ntyco_start(unsigned short port,msg_handler handler);
int ntyco_start_listeners(const kvs_listener_config_t *listeners,size_t listener_count);
int ntyco_start_runtime(const kvs_listener_config_t *listeners,size_t listener_count,const kvs_connector_config_t *connectors,size_t connector_count);
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

