
//ntyco.c


#include "nty_coroutine.h"
#include "server.h"
#include "memory_pool.h"
#include <arpa/inet.h>
#include<errno.h>
#define NTYCO_CONNECTION_CONTEXT_BLOCKS_PER_CHUNK 64
#define NTYCO_MAX_LISTENER_COUNT 2
#define NTYCO_MAX_CONNECTOR_COUNT 1 // 当前一台 Replica 只连接一个 Primary
typedef struct {
	unsigned short port;
	msg_handler handler;
	memory_pool_t connection_pool;  // 连接上下文专用池
}ntyco_listener_context_t;

typedef struct {
	int fd;
	msg_handler handler;
	kvs_connection_close_handler close_handler; // 连接关闭时通知业务层，可以为 NULL
	memory_pool_t *pool;
} ntyco_connection_context_t;

typedef struct {
    const char *host;                              // 要连接的目标地址
    unsigned short port;                           // 要连接的目标端口
    int fd;                                        // NtyCo 创建的连接 socket
    kvs_connection_open_handler open_handler;      // 连接成功回调
    msg_handler message_handler;                   // 收到数据后的处理函数
    kvs_connection_close_handler close_handler;    // 连接断开回调
} ntyco_connector_context_t;

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

    return 0;
}

static int ntyco_listener_context_init(
        ntyco_listener_context_t *context,
        const kvs_listener_config_t *config){

        if(context==NULL||config==NULL||config->port==0||config->handler==NULL){
                return -1;
        }

        context->port=config->port;
        context->handler=config->handler;

        return memory_pool_init(
                &context->connection_pool,
                sizeof(ntyco_connection_context_t),
                NTYCO_CONNECTION_CONTEXT_BLOCKS_PER_CHUNK
        );
}

static void ntyco_listener_context_destroy(
        ntyco_listener_context_t *context){

        if(context==NULL){
                return;
        }

        memory_pool_destory(&context->connection_pool);
}

void server_reader(void *arg) {
	ntyco_connection_context_t *context = (ntyco_connection_context_t *)arg;
	int fd=context->fd;
	msg_handler handler = context->handler;
	kvs_connection_close_handler close_handler=context->close_handler;
	memory_pool_t *pool = context->pool;
	if(pool!=NULL)memory_pool_free(pool,context);
	int ret = 0;//保存recv send的返回值
	kvs_input_buffer_t input={0};//跨循环 保存请求数
	kvs_output_buffer_t output={0};//保存当前链接尚未发送完的响应
	while (1) {
		int consumed_length=0;
		int available=BUFFER_LENGTH-input.length;//计算还能接受多少字节
		if(available<=0){
			break;
		}
		ret = nty_recv(fd,input.data+input.length,available, 0);
		if (ret > 0) {
			input.length+=ret;
			output.offset=0;
			output.length=handler(fd,input.data,input.length,output.data,BUFFER_LENGTH,&consumed_length);
			if (output.length<0){
				break;
			}
			if(kvs_input_buffer_consume(&input,consumed_length)<0){
				break;
			}
			if(output.length==0){
				continue;
			}
			while(output.offset<output.length){
				int remaining=output.length-output.offset;
				ret=nty_send(fd,output.data+output.offset,remaining,0);
				if(ret>0){
				output.offset+=ret;
				continue;
				}
				if(ret<0&&errno==EINTR){
					continue;
				}
				goto connection_closed; // 跳出两层循环，进入统一清理
			}
				output.length=0;
				output.offset=0;

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

	if(nty_connect(fd,(struct sockaddr *)&primary_address,sizeof(primary_address))!=0){
		fprintf(stderr,"failed to connect primary %s:%u\n",context->host,context->port);
		nty_close(fd);
		context->fd=-1;
		return;
	}
	kvs_output_buffer_t output={0};
	output.length=context->open_handler(fd,output.data,BUFFER_LENGTH);

	if(output.length<0||output.length>BUFFER_LENGTH){
		nty_close(fd);
		context->close_handler(fd);
		context->fd=-1;
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
		return;
	}

	printf("connect to primary %s:%u\n",context->host,context->port);

	ntyco_connection_context_t connection = {
		.fd =fd,
		.handler=context->message_handler,
		.close_handler=context->close_handler,
		.pool=NULL
	};

	server_reader(&connection);
	context->fd=-1;


}


void server(void *arg) {

	ntyco_listener_context_t *context=(ntyco_listener_context_t *)arg;
	unsigned short port=context->port;
	int fd = nty_socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return ;

	struct sockaddr_in local, remote;
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr*)&local, sizeof(struct sockaddr_in));

	listen(fd, 20);
	printf("listen port : %d\n", port);


	while (1) {
		socklen_t len = sizeof(struct sockaddr_in);
		int cli_fd = nty_accept(fd, (struct sockaddr*)&remote, &len);
		if(cli_fd<0){
			continue;
		}

		ntyco_connection_context_t *connection = memory_pool_alloc(&context->connection_pool);
		if(connection==NULL){
			nty_close(cli_fd);
			continue;
		}
		connection->fd=cli_fd;
		connection->handler=context->handler;
		connection->close_handler=NULL; // 普通客户端连接不需要复制断开通知
		connection->pool=&context->connection_pool;
		nty_coroutine *read_co;
		if(nty_coroutine_create(&read_co, server_reader, connection)!=0){
			nty_close(cli_fd);
			memory_pool_free(&context->connection_pool,connection);
		}

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
	nty_schedule_run();
	for(size_t i=0;i<initialized_count;i++){
		ntyco_listener_context_destroy(&contexts[i]);
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



