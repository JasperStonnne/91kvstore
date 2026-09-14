
//ntyco.c


#include "nty_coroutine.h"
#include "server.h"
#include <arpa/inet.h>
#include<errno.h>
typedef int (*msg_handler)(char *msg,int length,char *response,int response_capacity,int *consumed_length);
static msg_handler kvs_handler;

void server_reader(void *arg) {
	int fd = *(int *)arg;
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
			output.length=kvs_handler(input.data,input.length,output.data,BUFFER_LENGTH,&consumed_length);
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

	unsigned short port = *(unsigned short *)arg;
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


		nty_coroutine *read_co;
		nty_coroutine_create(&read_co, server_reader, &cli_fd);

	}

}




int ntyco_start(unsigned short port,msg_handler handler) {

	//int port = atoi(argv[1]);
	kvs_handler=handler;

	nty_coroutine *co = NULL;
	nty_coroutine_create(&co, server, &port);

	nty_schedule_run();

	return 0;
}




