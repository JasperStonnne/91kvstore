
//ntyco.c


#include "nty_coroutine.h"
#include "server.h"
#include "kvs_allocator.h"
#include "log.h"
#include "protocol.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include<errno.h>
#include <string.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <poll.h>
#define NTYCO_MAX_LISTENER_COUNT 2
static int ntyco_runtime_active;
#define NTYCO_MAX_CONNECTOR_COUNT 1 // 当前一台 Replica 只连接一个 Primary
#define NTYCO_STREAM_RETRY_INTERVAL_MS 1
#define NTYCO_READ_CHUNK_BYTES (64 * 1024)
#define NTYCO_STREAM_READ_BYTES (64 * 1024)
/* 待发送回复达到 64 KiB 时，先发送这一批。 */
#define NTYCO_REPLY_BATCH_THRESHOLD (64 * 1024)

typedef struct {
	unsigned short port;
	const char *bind_ip;/* 指向启动配置中的 IP；NULL 沿用 0.0.0.0。 */
	int listen_fd; /* 本监听器拥有的 socket；-1 表示尚未创建。 */
	msg_handler handler;
	kvs_connection_stream_handler stream_handler; // 当前监听器的可选流式输出函数
	msg_handler replication_handler;
    kvs_connection_stream_handler replication_stream_handler;
	kvs_connection_close_handler replication_close_handler;
}ntyco_listener_context_t;

typedef struct {
	int fd;
	msg_handler handler;
	kvs_connection_stream_handler stream_handler; // 该连接可选的流式输出函数
	kvs_connection_close_handler close_handler; // 连接关闭时通知业务层，可以为 NULL
	/* 1：由 kvs_malloc 申请；0：借用调用者的对象。 */
	int heap_owned;
	msg_handler replication_handler;
    kvs_connection_stream_handler replication_stream_handler;
	long long identification_deadline_ms;
	kvs_connection_close_handler replication_close_handler;
} ntyco_connection_context_t;

typedef struct{
    const char *host;                              // 要连接的目标地址
    unsigned short port;                           // 要连接的目标端口
    int fd;                                        // NtyCo 创建的连接 socket
    kvs_connection_open_handler open_handler;      // 连接成功回调
    msg_handler message_handler;                   // 收到数据后的处理函数
	kvs_connection_stream_handler stream_handler;
    kvs_connection_close_handler close_handler;    // 连接断开回调
} ntyco_connector_context_t;

#define NTYCO_IDENTIFICATION_TIMEOUT_MS 5000
#define NTYCO_IDENTIFICATION_RETRY_MS 10

/* 客户端和复制连接都立即发送短回复，避免批次末尾等待 TCP 合并。 */
static int ntyco_set_nodelay(int fd)
{
    int enabled = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0) {
        kvs_log_message(KVS_LOG_ERROR, "set TCP_NODELAY fd=%d: %s\n", fd, strerror(errno));
        return -1;
    }
    return 0;
}

static long long ntyco_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }

    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* 仅在连接协议尚未确定时使用。 */
static ssize_t ntyco_recv_before_deadline(
    int fd,
    void *buffer,
    size_t capacity,
    long long deadline_ms)
{
    while (1) {
        long long now = ntyco_monotonic_ms();
        if (now < 0) {
            return -1;
        }

        long long remaining = deadline_ms - now;
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        /* 直接进行非阻塞接收，避免进入 recv 的无限等待 hook。 */
        ssize_t received = syscall(
            SYS_recvfrom,
            fd, buffer, capacity, MSG_DONTWAIT,
            NULL, NULL
        );

        if (received >= 0) {
            return received;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }

        /* 暂无数据：登记短时休眠并让出协程，不占着 CPU 循环。 */
        long long delay =
            remaining < NTYCO_IDENTIFICATION_RETRY_MS
                ? remaining
                : NTYCO_IDENTIFICATION_RETRY_MS;

        nty_coroutine *co =
            nty_coroutine_get_sched()->curr_thread;

        nty_schedule_sched_sleepdown(co, (uint64_t)delay);
        nty_coroutine_yield(co);
    }
}

static int ntyco_connector_context_init(
    ntyco_connector_context_t *context,
    const kvs_connector_config_t *config)
{
    if (context == NULL ||
        config == NULL ||
        config->host == NULL ||
        config->host[0] == '\0' ||
        config->port == 0 ||
        config->open_handler == NULL ||
        config->message_handler == NULL ||
        config->close_handler == NULL) {
        return -1;
    }

    context->host = config->host;                       // 保存目标地址
    context->port = config->port;                       // 保存目标端口
    context->fd = -1;                                   // 当前尚未建立连接
    context->open_handler = config->open_handler;
    context->message_handler = config->message_handler;
    context->close_handler = config->close_handler;
	context->stream_handler = config->stream_handler;
    return 0;
}

static int ntyco_listener_context_init(
        ntyco_listener_context_t *context,
        const kvs_listener_config_t *config){

        if(context==NULL||config==NULL||config->port==0||config->handler==NULL){
                return -1;
        }

        context->port=config->port;
		context->bind_ip = config->bind_ip;
		context->listen_fd = -1;
        context->handler=config->handler;
		context->stream_handler=config->stream_handler;
		context->replication_handler = config->replication_handler;
		context->replication_stream_handler =config->replication_stream_handler;
		context->replication_close_handler =config->replication_close_handler;
		return 0;
}

static void ntyco_listener_context_destroy(
        ntyco_listener_context_t *context){

        if(context==NULL){
                return;
        }
		if (context->listen_fd >= 0) {
			close(context->listen_fd);
			context->listen_fd = -1;
		}
}

/*
 * 在协程启动之前，创建并绑定一个监听 socket。
 *
 * 成功：把 fd 交给 context->listen_fd，由 context_destroy() 负责关闭。
 * 失败：本函数关闭自己创建的 fd，context->listen_fd 仍保持 -1。
 */
static int ntyco_listener_open(ntyco_listener_context_t *context)
{
    /* 旧调用方没有指定 IP 时，维持原来的“监听所有网卡”。 */
    const char *ip =
        context->bind_ip != NULL ? context->bind_ip : "0.0.0.0";

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(context->port);

    if (inet_pton(AF_INET, ip, &local.sin_addr) != 1) {
        fprintf(stderr, "invalid listener IP: %s\n", ip);
        return -1;
    }

    /* nty_socket() 会建立 NtyCo 所需的非阻塞 socket。 */
    int fd = nty_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "failed to create listener for %s:%u\n",
                ip, context->port);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        int saved_errno = errno;
        fprintf(stderr, "failed to bind %s:%u: %s\n",
                ip, context->port, strerror(saved_errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 20) < 0) {
        int saved_errno = errno;
        fprintf(stderr, "failed to listen on %s:%u: %s\n",
                ip, context->port, strerror(saved_errno));
        close(fd);
        return -1;
    }

    /* 只有全部成功，才转交 fd 的所有权。 */
    context->listen_fd = fd;
    return 0;
}

static int ntyco_send_all(int fd,const char *data,int length)
{
    if(fd<0 || data==NULL || length<0){
        return -1;
    }

    int offset=0;

    while(offset<length){
        int ret = nty_send(fd, data + offset, length - offset, MSG_NOSIGNAL);


        if(ret>0){
            offset+=ret;
            continue;
        }

        if(ret<0 && errno==EINTR){
            continue;
		}

        return -1;
    }

    return 0;
}

/* 完整发送当前积累的回复，成功后重置有效长度。 */
static int ntyco_flush_output(int fd, kvs_output_buffer_t *output)
{
    /* 没有积累回复，不需要发送。 */
    if (output->length == 0) {
        return 0;
    }

    /* send_all 会循环发送，直到全部发完或发生错误。 */
    if (ntyco_send_all(fd, output->data, output->length) < 0) {
        return -1;
    }

    /*
     * 已全部发送，重置长度。
     * 保留分配的内存，供下一批回复复用。
     */
    output->length = 0;
    output->offset = 0;
    return 0;
}

/* stream 等待时仍处理完整控制帧；半帧留在原输入缓冲，不另建协议栈。 */
static int ntyco_stream_control(int fd, msg_handler handler,
                                 kvs_input_buffer_t *input, kvs_output_buffer_t *output)
{
    if (input->length == input->offset)
        return 0;
    int consumed = 0;
    int response = handler(fd, input->data + input->offset, input->length - input->offset, output, &consumed);
    if (response < 0 || response > output->capacity || consumed < 0 || consumed > input->length - input->offset ||
        (response > 0 && consumed == 0))
        return -1;
    if (kvs_input_buffer_consume(input, consumed) < 0)
        return -1;
    output->length = response;
    if (ntyco_flush_output(fd, output) < 0)
        return -1;
    return consumed > 0;
}

void server_reader(void *arg) {
    ntyco_connection_context_t *context =
        (ntyco_connection_context_t *)arg;

    int fd = context->fd;
    msg_handler handler = context->handler;
    kvs_connection_stream_handler stream_handler =
        context->stream_handler;
    kvs_connection_close_handler close_handler =
        context->close_handler;

    msg_handler replication_handler =
        context->replication_handler;
    kvs_connection_stream_handler replication_stream_handler =
        context->replication_stream_handler;

    /* 没有配置复制候选路径的监听器，继续沿用原处理方式。 */
	kvs_connection_close_handler replication_close_handler =context->replication_close_handler;
    int protocol_selected = (replication_handler == NULL);
	long long identification_deadline_ms =context->identification_deadline_ms;

	/* 所需参数已经复制到上面的局部变量，归还动态上下文。 */
	if (context->heap_owned) {
		kvs_free(context);
	}
	int ret = 0;//保存recv send的返回值
	kvs_input_buffer_t input={0};//跨循环 保存请求数
	kvs_output_buffer_t output={0};//保存当前链接尚未发送完的响应
	while (1) {
		int consumed_length=0;
		/* 保留尚未处理的数据；空间满时先扩容，再继续接收。 */
	if (kvs_input_buffer_ensure_space(&input, NTYCO_READ_CHUNK_BYTES) < 0) {
		goto connection_closed;
	}
	int available = input.capacity - input.length;
		if (!protocol_selected) {
		ret = ntyco_recv_before_deadline(
			fd,
			input.data + input.length,
			(size_t)available,
			identification_deadline_ms
		);
        } else {
            ret = nty_recv(
                fd,
                input.data + input.length,
                available,
                0
            );
        }
		if (ret > 0) {
			input.length+=ret;
		do{
			consumed_length=0;
			output.offset=0;
			    if (!protocol_selected) {
                kvs_connection_kind_t kind =
                    kvs_identify_connection(input.data + input.offset, input.length - input.offset);

                if (kind == KVS_CONNECTION_NEED_MORE) {
                    /* 半包不消费，退出内层循环，继续接收。 */
                    break;
                }

                if (kind == KVS_CONNECTION_INVALID) {
                    goto connection_closed;
                }

                if (kind == KVS_CONNECTION_REPLICA) {
                    handler = replication_handler;
                    stream_handler = replication_stream_handler;
					close_handler = replication_close_handler;
                }

                /* 客户端沿用默认处理器；选定后不再切换身份。 */
                protocol_selected = 1;
            }

			/* output.data 现在是指针，首次写响应前先分配初始空间。 */
			if (kvs_output_buffer_ensure_space(&output, BUFFER_LENGTH) < 0) {
				goto connection_closed;
			}

			/* handler 返回本条命令新生成的回复长度。 */
			int response_length = handler(
					fd,
					input.data + input.offset,
					input.length - input.offset,
					&output,
					&consumed_length
			);

			/* 新回复不能超过剩余空间。 */
			if (response_length < 0 ||
				response_length > output.capacity - output.length) {
					goto connection_closed;
			}

			/* 保留之前的长度，再加上本条回复长度。 */
			output.length += response_length;
			if(kvs_input_buffer_consume(&input,consumed_length)<0){
				goto connection_closed;
			}
			if (output.length == 0 && stream_handler == NULL) {
				continue;
			}

			/*
				* 普通客户端未达到阈值时，继续积累回复。
				* 这里的 continue 属于内部的 do...while 循环。
				*/
			if (stream_handler == NULL &&
				close_handler == NULL &&
				output.length < NTYCO_REPLY_BATCH_THRESHOLD) {
					continue;
			}

			/* 达到阈值或属于复制连接时，立即发送。 */
			if (ntyco_flush_output(fd, &output) < 0) {
					goto connection_closed;
			}

			if(stream_handler!=NULL){
                if (kvs_output_buffer_ensure_space(&output, NTYCO_STREAM_READ_BYTES) < 0) {
                    goto connection_closed;
                }
				while(1){
					int control = ntyco_stream_control(fd, handler, &input, &output);
					if (control < 0) goto connection_closed;
					if (control > 0) {
						consumed_length = 1; /* 让 stream 结束后也继续处理缓冲区中的尾帧。 */
						continue;
					}
					int stream_length=stream_handler(
						fd,
						output.data,
						NTYCO_STREAM_READ_BYTES
					);
				/*
				* -2 不是错误，只表示长期复制流当前没有新数据。
				* 当前协程休眠一小段时间，让出 CPU 后再检查。
				*/
				if(stream_length==KVS_STREAM_WAIT){
                    /* 非阻塞收控制帧；输入暂不能消费时限在一个 stream 块内。 */
                    ssize_t pending = -1;
                    errno = EAGAIN;
                    if (input.length - input.offset < NTYCO_STREAM_READ_BYTES) {
                        if (kvs_input_buffer_ensure_space(&input, 1) < 0)
                            goto connection_closed;
                        int available = input.capacity - input.length;
                        if (available > NTYCO_STREAM_READ_BYTES - input.length)
                            available = NTYCO_STREAM_READ_BYTES - input.length;
                        pending = syscall(SYS_recvfrom, fd, input.data + input.length,
                                          available, MSG_DONTWAIT, NULL, NULL);
                    }
                    if (pending > 0) {
                        input.length += (int)pending;
                        continue;
                    }

                    if (pending == 0) {
                        /* 对端已关闭连接。 */
                        goto connection_closed;
                    }

                    if (pending < 0 &&
                        errno != EAGAIN &&
                        errno != EWOULDBLOCK &&
                        errno != EINTR) {
                        goto connection_closed;
                    }
					/*
					* 某些 NtyCo 版本的非零 sleep 只登记定时器、不主动让出协程。
					* 在适配层明确完成这两步，避免复制流空闲时阻塞其他连接。
					*/
					nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;
					nty_schedule_sched_sleepdown(co, NTYCO_STREAM_RETRY_INTERVAL_MS);
					nty_coroutine_yield(co);
					continue;
				}
					if(stream_length<0 || stream_length>NTYCO_STREAM_READ_BYTES){
						goto connection_closed;
					}

					if(stream_length==0){
						break;
					}

					if(ntyco_send_all(
							fd,
							output.data,
							stream_length) < 0){
						goto connection_closed;
					}
				}
			}
	} while (input.length > input.offset && consumed_length > 0);

			/*
				* 内部循环结束后，必须发送剩余回复，
				* 然后才能等待下一次接收。
				*/
			if (ntyco_flush_output(fd, &output) < 0) {
					goto connection_closed;
			}
			} else if (ret == 0) {
					break;                              // 对端正常断开
			} else if (errno == EINTR) {
					continue;                           // 被信号中断，继续接收
			} else {
					break;                              // 其他接收错误
			}

	}
	connection_closed:
        nty_close(fd);                      // 所有退出路径只关闭一次 socket
		kvs_input_buffer_free(&input);
		kvs_output_buffer_free(&output);
        if(close_handler!=NULL){
                close_handler(fd);          // 复制连接断开时通知复制管理器
        }
}

static void ntyco_connector(void *arg)
{
	ntyco_connector_context_t *context = (ntyco_connector_context_t *)arg;
	if (context==NULL) return;

	struct sockaddr_in primary_address={0};
	primary_address.sin_family=AF_INET;
	primary_address.sin_port=htons(context->port);

	if(inet_pton(AF_INET,context->host,&primary_address.sin_addr)!=1){
		fprintf(stderr,"invalid primary IPv4 address: %s\n",context->host);
		return;
	}

	int fd=nty_socket(AF_INET,SOCK_STREAM,0);              // 创建一个受 NtyCo 调度的 TCP socket
    if(fd<0){
        fprintf(stderr,"failed to create primary connection socket\n");
        return;
    }

    if (ntyco_set_nodelay(fd) < 0) {
        nty_close(fd);
        return;
    }

	if(nty_connect(fd,(struct sockaddr *)&primary_address,sizeof(primary_address))!=0){
		fprintf(stderr,"failed to connect primary %s:%u\n",context->host,context->port);
		nty_close(fd);
		context->fd=-1;
		return;
	}
	kvs_output_buffer_t output={0};
	/* open_handler 要写入 output.data，先给它有效空间。 */
	if (kvs_output_buffer_ensure_space(&output, BUFFER_LENGTH) < 0) {
		nty_close(fd);
		context->close_handler(fd);
		context->fd = -1;
		kvs_output_buffer_free(&output);
		return;
	}
	output.length=context->open_handler(fd,output.data,BUFFER_LENGTH);

	if(output.length<0||output.length>BUFFER_LENGTH){
		nty_close(fd);
		context->close_handler(fd);
		context->fd=-1;
		kvs_output_buffer_free(&output);
		return;
	}
	while(output.offset<output.length){
		int remaining=output.length-output.offset;

		int ret=nty_send(fd,output.data+output.offset,remaining,0);

		if(ret>0){
			output.offset+=ret;
			continue;
		}
		if(ret<0&&errno==EINTR){
			continue;
		}
		nty_close(fd);
		context->close_handler(fd);
		context->fd=-1;
		kvs_output_buffer_free(&output);
		return;
	}
	/* 初始消息已发送完毕，后续接收由 server_reader 管理。 */
	kvs_output_buffer_free(&output);
	kvs_log_message(
    KVS_LOG_INFO, "connect to primary %s:%u\n",
    context->host, context->port
);

	ntyco_connection_context_t connection = {
		.fd =fd,
		.handler=context->message_handler,
		.stream_handler = context->stream_handler,
		.close_handler=context->close_handler,
        .heap_owned = 0
	};

	server_reader(&connection);
	context->fd=-1;


}


void server(void *arg) {

	ntyco_listener_context_t *context =
		(ntyco_listener_context_t *)arg;

	/*
	* socket 应已由 ntyco_start_runtime() 在启动协程前完成绑定。
	* 这里仅使用它接收连接，不再重复 socket()/bind()/listen()。
	*/
	int fd = context->listen_fd;
	if (fd < 0) {
		fprintf(stderr, "listener for port %u has no socket\n",
				context->port);
		return;
	}

	struct sockaddr_in remote;
	kvs_log_message(KVS_LOG_INFO, "listen port : %u\n", context->port);

	while (1) {
		socklen_t len = sizeof(struct sockaddr_in);
		int cli_fd = nty_accept(fd, (struct sockaddr*)&remote, &len);
		if(cli_fd<0){
			continue;
		}
        if (ntyco_set_nodelay(cli_fd) < 0) {
            nty_close(cli_fd);
            continue;
        }
		long long identification_deadline_ms = 0;

        if (context->replication_handler != NULL) {
            long long accepted_at = ntyco_monotonic_ms();

            if (accepted_at < 0) {
                nty_close(cli_fd);
                continue;
            }

            identification_deadline_ms =
                accepted_at + NTYCO_IDENTIFICATION_TIMEOUT_MS;
        }
		/* 只在 DEBUG 下记录连接建立；不打印每条请求或请求内容。 */
		kvs_log_message(KVS_LOG_DEBUG, "client accepted fd=%d\n", cli_fd);
	ntyco_connection_context_t *connection =kvs_malloc(sizeof(*connection));
		if(connection==NULL){
			nty_close(cli_fd);
			continue;
		}
		connection->fd=cli_fd;
		connection->identification_deadline_ms =identification_deadline_ms;
		connection->handler=context->handler;
		connection->stream_handler=context->stream_handler;
		connection->replication_handler = context->replication_handler;
		connection->replication_stream_handler =context->replication_stream_handler;
		connection->replication_close_handler =context->replication_close_handler;
		connection->close_handler=NULL; // 普通客户端连接不需要复制断开通知
		connection->heap_owned = 1; /* 由读协程负责释放。 */
		nty_coroutine *read_co;

		if(nty_coroutine_create(&read_co, server_reader, connection)!=0){
			nty_close(cli_fd);
			kvs_free(connection);
		}

	}

}


static void (*ntyco_maintenance_callback)(void);

void ntyco_set_maintenance(void (*callback)(void))
{
    ntyco_maintenance_callback = callback;
}

/* timer 在空闲 epoll 时也唤醒；显式 yield，不能只登记 sleep。 */
static void ntyco_maintenance(void *unused)
{
    (void)unused;
    while (ntyco_maintenance_callback != NULL) {
        ntyco_maintenance_callback();
        nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;
        nty_schedule_sched_sleepdown(co, 10);
        nty_coroutine_yield(co);
    }
}

int ntyco_start_runtime(const kvs_listener_config_t *listeners,size_t listener_count,const kvs_connector_config_t *connectors,size_t connector_count){
	if(listeners==NULL||listener_count==0||listener_count>NTYCO_MAX_LISTENER_COUNT){
		return -1;
	}//参数校验
	if(connector_count>NTYCO_MAX_CONNECTOR_COUNT||
	(connector_count>0&&connectors==NULL)){
		return -1; // 有连接器时必须提供对应配置
}
	ntyco_listener_context_t contexts[NTYCO_MAX_LISTENER_COUNT];
	ntyco_connector_context_t connector_contexts[NTYCO_MAX_CONNECTOR_COUNT];
	size_t initialized_count=0;
	for(size_t i=0;i<listener_count;i++){
		if(ntyco_listener_context_init(&contexts[i],&listeners[i])!=0){
			for(size_t j=0;j<initialized_count;j++){
				ntyco_listener_context_destroy(&contexts[j]);

			}
			return -1;
		}
		initialized_count++;
	}//将每份监听器config 转换成 NtyCo context
	        for(size_t i=0;i<connector_count;i++){
                if(ntyco_connector_context_init(
                        &connector_contexts[i],
                        &connectors[i])!=0){

                        // 连接器初始化失败，释放前面已经初始化好的监听器资源
                        for(size_t j=0;j<initialized_count;j++){
                                ntyco_listener_context_destroy(&contexts[j]);
                        }
                        return -1;
                }
        }//将每份连接器 config 转换成 NtyCo connector context

		/*
		* 先把所有监听端口绑定好，再创建协程。
		* Primary 有两个端口：任意一个失败，就释放已打开的端口，
		* 不允许服务器只启动一半。
		*/
		for (size_t i = 0; i < listener_count; ++i) {
			if (ntyco_listener_open(&contexts[i]) < 0) {
				for (size_t j = 0; j < initialized_count; ++j) {
					ntyco_listener_context_destroy(&contexts[j]);
				}
				return -1;
			}
		}

		for(size_t i=0;i<listener_count;i++){
			nty_coroutine *co = NULL;
		if(nty_coroutine_create(&co,server,&contexts[i])!=0){
			fprintf(stderr,
					"failed to create listener coroutine for port %u\n",
					contexts[i].port);

			for(size_t j=0;j<initialized_count;j++){
				ntyco_listener_context_destroy(&contexts[j]);
			}

		return -1;
	}
	}
	for(size_t i=0;i<connector_count;i++){
		nty_coroutine *co=NULL;

		if(nty_coroutine_create(
				&co,
				ntyco_connector,
				&connector_contexts[i])!=0){

				fprintf(stderr,
						"failed to create connector coroutine for %s:%u\n",
						connector_contexts[i].host,
						connector_contexts[i].port);

				for(size_t j=0;j<initialized_count;j++){
						ntyco_listener_context_destroy(&contexts[j]);
				}
				return -1;
		}
}//为每个主动连接任务创建一个连接器协程
    if (ntyco_maintenance_callback != NULL) {
        nty_coroutine *co;
        if (nty_coroutine_create(&co, ntyco_maintenance, NULL) != 0) {
            for (size_t i = 0; i < initialized_count; i++)
                ntyco_listener_context_destroy(&contexts[i]);
            return -1;
        }
    }
	ntyco_runtime_active = 1;
	nty_schedule_run();
	ntyco_runtime_active = 0;
	for(size_t i=0;i<initialized_count;i++){
		ntyco_listener_context_destroy(&contexts[i]);
	}
	return 0;
}

struct kvs_dynamic_connector {
    kvs_dynamic_connector_config_t config;
    char host[256];
    int fd;
    int cancelled;
};

static void ntyco_dynamic_pause(void)
{
    nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;
    nty_schedule_sched_sleepdown(co, 1);
    nty_coroutine_yield(co);
}

void ntyco_shutdown_connection(int fd)
{
    if (fd >= 0)
        (void)syscall(SYS_shutdown, fd, SHUT_RDWR);
}

void ntyco_cancel_dynamic(kvs_dynamic_connector_t **handle)
{
    if (handle == NULL || *handle == NULL)
        return;
    kvs_dynamic_connector_t *connection = *handle;
    *handle = NULL;
    connection->cancelled = 1;
    ntyco_shutdown_connection(connection->fd);
}

static int ntyco_dynamic_connect(kvs_dynamic_connector_t *connection)
{
    struct sockaddr_in address = { .sin_family = AF_INET,
        .sin_port = htons(connection->config.port) };
    if (inet_pton(AF_INET, connection->host, &address.sin_addr) != 1)
        return -1;
    connection->fd = (int)syscall(SYS_socket, AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (connection->fd < 0 || ntyco_set_nodelay(connection->fd) < 0)
        return -1;
    int enabled = 1, idle = 5, interval = 1, count = 3;
    if (setsockopt(connection->fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(connection->fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0 ||
        setsockopt(connection->fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0 ||
        setsockopt(connection->fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0)
        return -1;
    if (syscall(SYS_connect, connection->fd, &address, sizeof(address)) == 0)
        return 0;
    if (errno != EINPROGRESS)
        return -1;
    long long started = ntyco_monotonic_ms();
    while (!connection->cancelled) {
        struct pollfd ready = { .fd = connection->fd, .events = POLLOUT };
        if (poll(&ready, 1, 0) > 0) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(connection->fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
                return -1;
            if (error) { errno = error; return -1; }
            return 0;
        }
        if (started < 0 || ntyco_monotonic_ms() - started >= 3000) {
            errno = ETIMEDOUT;
            return -1;
        }
        ntyco_dynamic_pause();
    }
    errno = ECANCELED;
    return -1;
}

static int ntyco_dynamic_flush(kvs_dynamic_connector_t *connection, kvs_output_buffer_t *output)
{
    if (output->offset < output->length) {
        ssize_t bytes = syscall(SYS_sendto, connection->fd, output->data + output->offset,
            output->length - output->offset, MSG_DONTWAIT | MSG_NOSIGNAL, NULL, 0);
        if (bytes < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        if (bytes == 0)
            return -1;
        output->offset += (int)bytes;
    }
    if (output->offset != output->length)
        return 0;
    output->offset = output->length = 0;
    return 1;
}

static void ntyco_dynamic_run(void *arg)
{
    kvs_dynamic_connector_t *connection = arg;
    kvs_dynamic_connector_config_t *config = &connection->config;
    kvs_input_buffer_t input = {0};
    kvs_output_buffer_t output = {0};
    if (connection->cancelled)
        goto done;
    if (ntyco_dynamic_connect(connection) < 0) {
        kvs_log_message(KVS_LOG_ERROR, "network: dynamic connect %s:%u failed: %s\n",
                        connection->host, config->port, strerror(errno));
        goto done;
    }
    if (connection->cancelled || kvs_output_buffer_ensure_space(&output, BUFFER_LENGTH) < 0)
        goto done;
    output.length = config->opened(config->context, config->session, connection->fd,
                                    output.data, output.capacity);
    if (output.length < 0 || output.length > output.capacity)
        goto done;
    while (!connection->cancelled) {
        int consumed = 0;
        /* 零长度回调用于业务超时检查；不伪造输入消息。 */
        if (config->message(config->context, config->session, connection->fd,
                            NULL, 0, &output, &consumed) < 0)
            break;
        int flushed = ntyco_dynamic_flush(connection, &output);
        if (flushed < 0)
            break;
        if (flushed) {
            /* 满缓冲仍先解析已有帧，避免公平调度的帧数预算触发误断开。 */
            if ((size_t)(input.length - input.offset) < config->max_input_bytes &&
                kvs_input_buffer_ensure_space(&input, 1) < 0)
                break;
            size_t available = (size_t)(input.capacity - input.length);
            if (available > config->max_input_bytes - (size_t)(input.length - input.offset))
                available = config->max_input_bytes - (size_t)(input.length - input.offset);
            ssize_t bytes = available ? syscall(SYS_recvfrom, connection->fd, input.data + input.length,
                                    available, MSG_DONTWAIT, NULL, NULL) : -1;
            if (!available)
                errno = EAGAIN;
            if (bytes == 0)
                break;
            if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                break;
            if (bytes > 0)
                input.length += (int)bytes;
            for (int budget = 0; budget < 32 && input.length > input.offset && !connection->cancelled; budget++) {
                if (kvs_output_buffer_ensure_space(&output, BUFFER_LENGTH) < 0)
                    goto done;
                kvs_output_buffer_t response = { .data = output.data + output.length,
                    .capacity = output.capacity - output.length };
                int reply = config->message(config->context, config->session, connection->fd,
                    input.data + input.offset, input.length - input.offset, &response, &consumed);
                if (reply < 0 || reply > response.capacity || consumed < 0 || consumed > input.length - input.offset)
                    goto done;
                output.length += reply;
                if (kvs_input_buffer_consume(&input, consumed) < 0)
                    goto done;
                if (!consumed) {
                    if ((size_t)(input.length - input.offset) >= config->max_input_bytes)
                        goto done;
                    break;
                }
            }
        }
        ntyco_dynamic_pause();
    }
done:
    if (connection->fd >= 0)
        (void)syscall(SYS_close, connection->fd); /* 唯一 close 点，取消接口不关闭 FD。 */
    kvs_input_buffer_free(&input);
    kvs_output_buffer_free(&output);
    config->closed(config->context, config->session, connection->fd);
    kvs_free(connection);
}

int ntyco_connect_dynamic(const kvs_dynamic_connector_config_t *config,
                           kvs_dynamic_connector_t **handle)
{
    struct in_addr address;
    if (!ntyco_runtime_active || nty_coroutine_get_sched()->curr_thread == NULL ||
        config == NULL || handle == NULL || *handle != NULL || config->host == NULL ||
        strlen(config->host) >= 256 || inet_pton(AF_INET, config->host, &address) != 1 ||
        config->port == 0 || config->opened == NULL || config->message == NULL ||
        config->closed == NULL || config->max_input_bytes == 0 || config->max_input_bytes > 1024 * 1024) {
        errno = EINVAL;
        return -1;
    }
    kvs_dynamic_connector_t *connection = kvs_malloc(sizeof(*connection));
    if (connection == NULL)
        return -1;
    *connection = (kvs_dynamic_connector_t){ .config = *config, .fd = -1 };
    strcpy(connection->host, config->host);
    connection->config.host = connection->host;
    *handle = connection;
    nty_coroutine *co;
    if (nty_coroutine_create(&co, ntyco_dynamic_run, connection) != 0) {
        *handle = NULL;
        kvs_free(connection);
        return -1;
    }
    return 0;
}

int ntyco_start_listeners(
        const kvs_listener_config_t *listeners,
        size_t listener_count){

        return ntyco_start_runtime(
                listeners,
                listener_count,
                NULL, // 只启动监听器，不启动主动连接器
                0
        );
}

int ntyco_start(unsigned short port,msg_handler handler) {
    kvs_listener_config_t listener={
        .port=port,
        .handler=handler
    };

    return ntyco_start_listeners(&listener,1);
}



