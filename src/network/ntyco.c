
//ntyco.c


#include "nty_coroutine.h"
#include "server.h"
#include "memory_pool.h"
#include <arpa/inet.h>
#include<errno.h>
#define NTYCO_CONNECTION_CONTEXT_BLOCKS_PER_CHUNK 64
#define NTYCO_MAX_LISTENER_COUNT 2
typedef struct {
	unsigned short port;
	msg_handler handler;
	memory_pool_t connection_pool;  // 连接上下文专用池
}ntyco_listener_context_t;

typedef struct {
	int fd;
	msg_handler handler;
	memory_pool_t *pool;
} ntyco_connection_context_t;
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
	memory_pool_t *pool = context->pool;
	memory_pool_free(pool,context);
	int ret = 0;//保存recv send的返回值
	kvs_input_buffer_t input={0};//跨循环 保存请求数
	kvs_output_buffer_t output={0};//保存当前链接尚未发送完的响应
	while (1) {
		int consumed_length=0;
		int available=BUFFER_LENGTH-input.length;//计算还能接受多少字节
		if(available<=0){
			close(fd);
			break;
		}
		ret = recv(fd,input.data+input.length,available, 0);
		if (ret > 0) {
			input.length+=ret;
			output.offset=0;
			output.length=handler(input.data,input.length,output.data,BUFFER_LENGTH,&consumed_length);
			if (output.length<0){
				close(fd);
				break;
			}
			if(output.length==0){
				continue;
			}
			if(kvs_input_buffer_consume(&input,consumed_length)<0){
				close(fd);
				break;
			}

			while(output.offset<output.length){
				int remaining=output.length-output.offset;
				ret=send(fd,output.data+output.offset,remaining,0);
				if(ret>0){
				output.offset+=ret;
				continue;
				}
				if(ret<0&&errno==EINTR){
					continue;
				}
				close(fd);
				return;
			}
				output.length=0;
				output.offset=0;

		} else if (ret == 0) {
			close(fd);
			break;
		}

	}
}



void server(void *arg) {

	ntyco_listener_context_t *context=(ntyco_listener_context_t *)arg;
	unsigned short port=context->port;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
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
		int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);
		if(cli_fd<0){
			continue;
		}

		ntyco_connection_context_t *connection = memory_pool_alloc(&context->connection_pool);
		if(connection==NULL){
			close(cli_fd);
			continue;
		}
		connection->fd=cli_fd;
		connection->handler=context->handler;
		connection->pool=&context->connection_pool;
		nty_coroutine *read_co;
		if(nty_coroutine_create(&read_co, server_reader, connection)!=0){
			close(cli_fd);
			memory_pool_free(&context->connection_pool,connection);
		}

	}

}


int ntyco_start_listeners(const kvs_listener_config_t *listeners,size_t listener_count){
	if(listeners==NULL||listener_count==0||listener_count>NTYCO_MAX_LISTENER_COUNT){
		return -1;
	}//参数校验

	ntyco_listener_context_t contexts[NTYCO_MAX_LISTENER_COUNT];
	size_t initialized_count=0;
	for(size_t i=0;i<listener_count;i++){
		if(ntyco_listener_context_init(&contexts[i],&listeners[i])!=0){
			for(size_t j=0;j<initialized_count;j++){
				ntyco_listener_context_destroy(&contexts[j]);

			}
			return -1;
		}
		initialized_count++;
	}//将每份通用 config 转换成 NtyCo context
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
	nty_schedule_run();
	for(size_t i=0;i<initialized_count;i++){
		ntyco_listener_context_destroy(&contexts[i]);
	}
	return 0;
}


int ntyco_start(unsigned short port,msg_handler handler) {
    kvs_listener_config_t listener={
        .port=port,
        .handler=handler
    };

    return ntyco_start_listeners(&listener,1);
}



