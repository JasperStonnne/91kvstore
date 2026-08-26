//proactor.c

#include<stdio.h>
#include<liburing.h>
#include<netinet/in.h>
#include<string.h>
#include<unistd.h>
#include "server.h"

#define EVENT_ACCEPT 0
#define EVENT_READ 1
#define EVENT_WRITE 2

extern int kvs_protocol(char *msg,int length,char *response);
typedef int (*msg_handler)(char *msg,int length,char *response);
static msg_handler kvs_handler;

struct conn_info {
    int fd;
    int event;

};




#define ENTRIES_LENGTH 1024
#define BUFFER_LENGTH 1024

int set_event_send(struct io_uring *ring,int sockfd,void *buf,size_t len,int flags){

    struct io_uring_sqe *sqe =io_uring_get_sqe(ring);//把队列理解成数组 就是一个头 返回的时候是一个mmap 在iouring setup的时候 在内核里面已经分配好了一部分空间 用户空间可以直接用 从而少了一步从用户空间copy到内核空间

    struct conn_info accept_info={
        .fd=sockfd,
        .event=EVENT_WRITE,
    };
    
    io_uring_prep_send(sqe,sockfd,buf,len,flags);//提交一个请求 准备prepare 往submitqueue里提交节点 accept三个参数 这里是五个参数 底层调用register
    memcpy(&sqe->user_data,&accept_info,sizeof(struct conn_info));



}

int set_event_recv(struct io_uring *ring,int sockfd,void *buf,size_t len,int flags){

    struct io_uring_sqe *sqe =io_uring_get_sqe(ring);//把队列理解成数组 就是一个头 返回的时候是一个mmap 在iouring setup的时候 在内核里面已经分配好了一部分空间 用户空间可以直接用 从而少了一步从用户空间copy到内核空间

    struct conn_info accept_info={
        .fd=sockfd,
        .event=EVENT_READ,
    };
    
    io_uring_prep_recv(sqe,sockfd,buf,len,flags);//提交一个请求 准备prepare 往submitqueue里提交节点 accept三个参数 这里是五个参数 底层调用register
    memcpy(&sqe->user_data,&accept_info,sizeof(struct conn_info));



}


int set_event_accept(struct io_uring *ring,int sockfd,struct sockaddr *addr, socklen_t *addrlen,int flags){

    struct io_uring_sqe *sqe =io_uring_get_sqe(ring);//把队列理解成数组 就是一个头 返回的时候是一个mmap 在iouring setup的时候 在内核里面已经分配好了一部分空间 用户空间可以直接用 从而少了一步从用户空间copy到内核空间

    struct conn_info accept_info={
        .fd=sockfd,
        .event=EVENT_ACCEPT,
    };
    
    io_uring_prep_accept(sqe,sockfd,(struct sockaddr*)addr,addrlen,flags);//提交一个请求 准备prepare 往submitqueue里提交节点 accept三个参数 这里是五个参数 底层调用register
    memcpy(&sqe->user_data,&accept_info,sizeof(struct conn_info));



}


int p_init_server(unsigned short port) {

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in servaddr;
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
	servaddr.sin_port = htons(port); // 0-1023, 

	if (-1 == bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr))) {
		printf("bind failed: %s\n", strerror(errno));
	}

	listen(sockfd, 10);
	//printf("listen finshed: %d\n", sockfd); // 3 

	return sockfd;

}



int proactor_start(unsigned short port,msg_handler handler){
    int sockfd=p_init_server(port);
    kvs_handler=handler;
   
     struct io_uring_params params;
     memset(&params,0,sizeof(params));

     struct  io_uring ring;
     io_uring_queue_init_params(ENTRIES_LENGTH,&ring,&params);//作用是构建 submitqueue和 completequeue
    //有setup哦 就是底层的iouring系统调用
     
 #if 0    
     struct sockaddr_in  clientaddr;
	 socklen_t len = sizeof(clientaddr);
     accept(sockfd,(struct sockaddr*)&clientaddr,&len);//同步的写法
#else

    struct sockaddr_in  clientaddr;
    socklen_t len = sizeof(clientaddr);
    set_event_accept(&ring,sockfd,(struct sockaddr*)&clientaddr,&len,0);




#endif

    char buffer[BUFFER_LENGTH]={0};
    char response[BUFFER_LENGTH]={0};

    while(1){

        io_uring_submit(&ring);//submit 里面的系统调用是enter 把内核里的事件提交进去

        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring,&cqe);//取compseq的开始位置 为什么带地址 是从函数内部反回来了 不要修改副本就是地址
        struct io_uring_cqe *cqes[128];
        int nready=io_uring_peek_batch_cqe(&ring,cqes,128);//一次性从ring里面 拿出来放到cqe里面 128个 返回会带一个参数 像nready一样

        int i=0;
        for(i=0;i<nready;i++){

            
            struct io_uring_cqe *entries=cqes[i];
            struct conn_info result;
            memcpy(&result,&entries->user_data,sizeof(struct conn_info));


            if(result.event==EVENT_ACCEPT){
            //printf("io_uring_peek_batch_cqe EVENT_ACCEPT\n");
            set_event_accept(&ring,sockfd,(struct sockaddr*)&clientaddr,&len,0);

            //printf("set_event_accept\n");
            
             int connfd=entries->res;
             

             set_event_recv(&ring,connfd,buffer,BUFFER_LENGTH,0);







            }else if(result.event==EVENT_READ){
                int ret=entries->res;
                

                if(ret==0){
                    close(result.fd);

                }else if(ret>0){
                    //printf("set_event_recv ret: %d,%s\n",ret,buffer);
                    //int kvs_protocol(char *msg,int length,char *response);
                    ret=kvs_handler(buffer,ret,response);
                    set_event_send(&ring,result.fd,response,ret,0);


                }
                
            }else if (result.event==EVENT_WRITE){
               int ret=entries->res; 
                //printf("set_event_send ret: %d,%s\n",ret,buffer);
                set_event_recv(&ring,result.fd,buffer,BUFFER_LENGTH,0);

            }



        }
        io_uring_cq_advance(&ring,nready);



    }

}