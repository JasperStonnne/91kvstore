
//proactor.c

#include<stdio.h>
#include<liburing.h>
#include<netinet/in.h>
#include<string.h>
#include<unistd.h>
#include <stdlib.h>
#include<errno.h>
#include <arpa/inet.h>
#include "server.h"

#define EVENT_ACCEPT 0
#define EVENT_READ 1
#define EVENT_WRITE 2
/*
 * ONLINE 状态暂时没有新 AOF 时，
 * 通过 io_uring timeout 事件再次检查复制流。
 */
#define EVENT_STREAM_RETRY 3

/* ONLINE 状态下每 100ms 检查一次新 AOF。 */
#define KVS_STREAM_RETRY_INTERVAL_MS 100
static struct conn
*proactor_connections[CONNECTION_SIZE];

/*
 * 关闭并释放一条已经注册到 Proactor 的连接。
 */
static void proactor_close_connection(int fd)
{
    if(fd<0 || fd>=CONNECTION_SIZE){
        return;
    }

    struct conn *connection=
        proactor_connections[fd];

    /*
     * 可能是已经处理过的旧完成事件。
     * 不能再次关闭这个 fd。
     */
    if(connection==NULL){
        return;
    }

    kvs_connection_close_handler close_handler=
        connection->close_handler;

    proactor_connections[fd]=NULL;

    close(fd);
    free(connection);

    if(close_handler!=NULL){
        close_handler(fd);
    }
}

struct conn_info {
    int fd;
    int event;

};




#define ENTRIES_LENGTH 1024


int set_event_send(struct io_uring *ring,int sockfd,void *buf,size_t len,int flags){

    struct io_uring_sqe *sqe =io_uring_get_sqe(ring);//把队列理解成数组 就是一个头 返回的时候是一个mmap 在iouring setup的时候 在内核里面已经分配好了一部分空间 用户空间可以直接用 从而少了一步从用户空间copy到内核空间
    if(sqe==NULL){
        return -1;
    }
    struct conn_info accept_info={
        .fd=sockfd,
        .event=EVENT_WRITE,
    };

    io_uring_prep_send(sqe,sockfd,buf,len,flags);//提交一个请求 准备prepare 往submitqueue里提交节点 accept三个参数 这里是五个参数 底层调用register
    memcpy(&sqe->user_data,&accept_info,sizeof(struct conn_info));

    return 0;
}

int set_event_recv(struct io_uring *ring,int sockfd,void *buf,size_t len,int flags){

    struct io_uring_sqe *sqe =io_uring_get_sqe(ring);//把队列理解成数组 就是一个头 返回的时候是一个mmap 在iouring setup的时候 在内核里面已经分配好了一部分空间 用户空间可以直接用 从而少了一步从用户空间copy到内核空间
    if (sqe==NULL){
        return -1;
    }
    struct conn_info accept_info={
        .fd=sockfd,
        .event=EVENT_READ,
    };

    io_uring_prep_recv(sqe,sockfd,buf,len,flags);//提交一个请求 准备prepare 往submitqueue里提交节点 accept三个参数 这里是五个参数 底层调用register
    memcpy(&sqe->user_data,&accept_info,sizeof(struct conn_info));


    return 0;
}


int set_event_accept(struct io_uring *ring,int sockfd,struct sockaddr *addr, socklen_t *addrlen,int flags){

    struct io_uring_sqe *sqe =io_uring_get_sqe(ring);//把队列理解成数组 就是一个头 返回的时候是一个mmap 在iouring setup的时候 在内核里面已经分配好了一部分空间 用户空间可以直接用 从而少了一步从用户空间copy到内核空间
    if(sqe==NULL){
        return -1;
    }
    struct conn_info accept_info={
        .fd=sockfd,
        .event=EVENT_ACCEPT,
    };

    io_uring_prep_accept(sqe,sockfd,(struct sockaddr*)addr,addrlen,flags);//提交一个请求 准备prepare 往submitqueue里提交节点 accept三个参数 这里是五个参数 底层调用register
    memcpy(&sqe->user_data,&accept_info,sizeof(struct conn_info));


    return 0;
}

/*
 * 为一条 ONLINE 复制连接提交延时重试任务。
 *
 * 100ms 后 io_uring 会生成 EVENT_STREAM_RETRY 完成事件，
 * result.fd 用来确定应该重新检查哪条连接。
 */
static int set_event_stream_retry(
    struct io_uring *ring,
    int sockfd)
{
    if(ring==NULL || sockfd<0){
        return -1;
    }

    struct io_uring_sqe *sqe=
        io_uring_get_sqe(ring);

    if(sqe==NULL){
        return -1;
    }

    /*
     * io_uring_prep_timeout 保存的是 timespec 地址，
     * 因此该对象必须在函数返回后继续存在，不能使用普通局部变量。
     */
    static struct __kernel_timespec timeout={
        .tv_sec=0,
        .tv_nsec=
            KVS_STREAM_RETRY_INTERVAL_MS*1000000LL
    };

    struct conn_info retry_info={
        .fd=sockfd,
        .event=EVENT_STREAM_RETRY
    };

    /*
     * count=0：只按时间等待；
     * flags=0：使用相对时间，即从提交后开始等待100ms。
     */
    io_uring_prep_timeout(
        sqe,
        &timeout,
        0,
        0
    );

    memcpy(
        &sqe->user_data,
        &retry_info,
        sizeof(retry_info)
    );

    return 0;
}


/*
 * 检查复制流并安排下一项异步操作。
 *
 * 返回值：
 *  1：已经提交 SEND 或 TIMEOUT
 *  0：没有 stream_handler，或者流已经结束
 * -1：处理失败
 */
static int proactor_schedule_stream_output(
    struct io_uring *ring,
    struct conn *connection)
{
    if(ring==NULL || connection==NULL){
        return -1;
    }

    if(connection->stream_handler==NULL){
        return 0;
    }

    int stream_length=connection->stream_handler(
        connection->fd,
        connection->output.data,
        BUFFER_LENGTH
    );

    if(stream_length>BUFFER_LENGTH ||
       (stream_length<0 &&
        stream_length!=KVS_STREAM_WAIT)){
        return -1;
    }

    if(stream_length>0){
        connection->output.offset=0;
        connection->output.length=stream_length;

        if(set_event_send(
                ring,
                connection->fd,
                connection->output.data,
                connection->output.length,
                0)<0){
            return -1;
        }

        return 1;
    }

    if(stream_length==KVS_STREAM_WAIT){
        if(set_event_stream_retry(
                ring,
                connection->fd)<0){
            return -1;
        }

        return 1;
    }

    return 0;
}

/*
 * 处理当前用户输入缓冲区里已经存在的数据。
 *
 * 返回 0：处理成功，或者需要等待更多网络数据。
 * 返回 -1：协议处理或缓冲区消费失败。
 */
static int proactor_process_input(
    struct conn *connection)
{
    if(connection==NULL ||
       connection->handler==NULL){
        return -1;
    }

    while(connection->input.length>0){
        int consumed_length=0;

        connection->output.offset=0;
        connection->output.length=
            connection->handler(
                connection->fd,
                connection->input.data,
                connection->input.length,
                connection->output.data,
                BUFFER_LENGTH,
                &consumed_length
            );

        if(connection->output.length<0 ||
           connection->output.length>BUFFER_LENGTH){
            return -1;
        }

        if(kvs_input_buffer_consume(
                &connection->input,
                consumed_length)<0){
            return -1;
        }

        /*
         * 已产生响应，需要先让网络层发送。
         */
        if(connection->output.length>0){
            break;
        }

        /*
         * 本轮没有消费数据，说明当前协议数据还不完整。
         * 等待下一次 RECV，避免在这里无限循环。
         */
        if(consumed_length<=0){
            break;
        }
    }

    return 0;
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

static int proactor_register_listener(
    struct io_uring *ring,
    const kvs_listener_config_t *listener)
{
    if(ring==NULL ||
       listener==NULL ||
       listener->port==0 ||
       listener->handler==NULL){
        return -1;
    }

    int sockfd=p_init_server(listener->port);
    if(sockfd<0 || sockfd>=CONNECTION_SIZE){
        if(sockfd>=0){
            close(sockfd);
        }
        return -1;
    }

    struct conn *listener_connection=
        calloc(1,sizeof(*listener_connection));

    if(listener_connection==NULL){
        close(sockfd);
        return -1;
    }

    listener_connection->fd=sockfd;
    listener_connection->handler=listener->handler;
    listener_connection->stream_handler=
        listener->stream_handler;

    proactor_connections[sockfd]=listener_connection;

    if(set_event_accept(
            ring,
            sockfd,
            NULL,
            NULL,
            0)<0){

        proactor_connections[sockfd]=NULL;
        free(listener_connection);
        close(sockfd);
        return -1;
    }

    printf("listen port : %u\n",listener->port);
    return 0;
}

/*
 * 主动连接远端服务器，并把连接注册到 Proactor。
 *
 * Replica 使用它连接 Primary 的复制端口。
 */
static int proactor_register_connector(
    struct io_uring *ring,
    const kvs_connector_config_t *connector)
{
    if(ring==NULL ||
       connector==NULL ||
       connector->host==NULL ||
       connector->host[0]=='\0' ||
       connector->port==0 ||
       connector->open_handler==NULL ||
       connector->message_handler==NULL){
        return -1;
    }

    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0){
        return -1;
    }

    struct sockaddr_in remote_address={0};
    remote_address.sin_family=AF_INET;
    remote_address.sin_port=htons(connector->port);

    if(inet_pton(
            AF_INET,
            connector->host,
            &remote_address.sin_addr)!=1){

        close(fd);
        return -1;
    }

    /*
     * 当前在进入 io_uring 事件循环前建立连接。
     * 连接成功后，后续 SEND/RECV 都交给 io_uring。
     */
    if(connect(
            fd,
            (struct sockaddr *)&remote_address,
            sizeof(remote_address))!=0){

        close(fd);
        return -1;
    }

    if(fd>=CONNECTION_SIZE){
        close(fd);
        return -1;
    }

    struct conn *connection=
        calloc(1,sizeof(*connection));

    if(connection==NULL){
        close(fd);
        return -1;
    }

    connection->fd=fd;
    connection->handler=
        connector->message_handler;
    connection->stream_handler=NULL;
    connection->close_handler=
        connector->close_handler;

    proactor_connections[fd]=connection;

    /*
     * TCP 连接建立后，让复制模块生成第一条 PING。
     */
    connection->output.length=
        connector->open_handler(
            fd,
            connection->output.data,
            BUFFER_LENGTH
        );
    connection->output.offset=0;

    if(connection->output.length<0 ||
       connection->output.length>BUFFER_LENGTH){

        proactor_connections[fd]=NULL;
        close(fd);

        if(connection->close_handler!=NULL){
            connection->close_handler(fd);
        }

        free(connection);
        return -1;
    }

    /*
     * open_handler 产生了 PING，先提交 SEND；
     * 如果没有产生数据，则直接等待 Primary 输入。
     */
    int event_result;

    if(connection->output.length>0){
        event_result=set_event_send(
            ring,
            fd,
            connection->output.data,
            connection->output.length,
            0
        );
    }else{
        event_result=set_event_recv(
            ring,
            fd,
            connection->input.data,
            BUFFER_LENGTH,
            0
        );
    }

    if(event_result<0){
        proactor_connections[fd]=NULL;
        close(fd);

        if(connection->close_handler!=NULL){
            connection->close_handler(fd);
        }

        free(connection);
        return -1;
    }

    printf(
        "connect to primary %s:%u\n",
        connector->host,
        connector->port
    );

    return 0;
}

static int proactor_run(const kvs_listener_config_t *listeners, size_t listener_count,const kvs_connector_config_t *connectors,size_t connector_count)
{
    if(listeners==NULL ||
    listener_count==0 ||
    (connector_count>0 && connectors==NULL)){
        return -1;
    }

    struct io_uring_params params;
    memset(&params,0,sizeof(params));

    struct io_uring ring;
    if(io_uring_queue_init_params(
            ENTRIES_LENGTH,
            &ring,
            &params)<0){
        return -1;
    }

    /*
     * 所有监听器共用同一个 io_uring。
     * Primary 会在这里分别注册客户端端口和复制端口。
     */
for(size_t i=0;i<listener_count;i++){
    if(proactor_register_listener(
            &ring,
            &listeners[i])<0){

        io_uring_queue_exit(&ring);
        return -1;
    }
}

/*
 * Replica 在进入事件循环前主动连接 Primary。
 */
for(size_t i=0;i<connector_count;i++){
    if(proactor_register_connector(
            &ring,
            &connectors[i])<0){

        io_uring_queue_exit(&ring);
        return -1;
    }
}

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
            set_event_accept(&ring,result.fd,NULL,NULL,0);

            //printf("set_event_accept\n");

             int connfd=entries->res;
             if(connfd<0){
                continue;
             }
             if(connfd>=CONNECTION_SIZE){
                close(connfd);
                continue;
             }

             /*
            * result.fd 是产生这次连接的监听 fd。
            * 通过它找到监听器绑定的协议。
            */
            struct conn *listener=
                proactor_connections[result.fd];

            if(listener==NULL ||
            listener->handler==NULL){
                    close(connfd);
                    continue;
            }

             struct conn *connection =calloc(1,sizeof(*connection));
             if(connection==NULL){
                close(connfd);
                continue;
             }
             connection->fd=connfd;
             connection->handler=listener->handler;
             connection->stream_handler=listener->stream_handler;
             proactor_connections[connfd]=connection;


             set_event_recv(&ring,connfd,connection->input.data,BUFFER_LENGTH,0);







            }else if(result.event==EVENT_READ){
                int ret=entries->res;

                if(result.fd<0||result.fd>=CONNECTION_SIZE){
                    continue;
                }

                struct conn *connection=proactor_connections[result.fd];

                if(connection==NULL){
                    continue;
                }


                if(ret==0){
                    proactor_close_connection(result.fd);
                    continue;

                }else if(ret>0){
                    //printf("set_event_recv ret: %d,%s\n",ret,buffer);
                    connection->input.length+=ret;
                if(proactor_process_input(connection)<0){
                    proactor_close_connection(result.fd);
                    continue;
                }
            if(connection->output.length==0){
                int available=BUFFER_LENGTH-connection->input.length;//计算剩下的空间
                if(available<=0){
                    proactor_close_connection(result.fd);
                    continue;
                }

                set_event_recv(&ring,result.fd,connection->input.data+connection->input.length,available,0);

                continue;

            }
                if(set_event_send(&ring,result.fd,connection->output.data,connection->output.length,0)<0){
                    proactor_close_connection(result.fd);
                }

                }

            }else if (result.event==EVENT_WRITE){
               int ret=entries->res;

                if(result.fd<0||result.fd>=CONNECTION_SIZE){
                    continue;
                }
                struct conn *connection=proactor_connections[result.fd];
                if(connection==NULL){
                    continue;
                }
                if(ret==-EAGAIN||ret==-EINTR){
                    int remaining=connection->output.length-connection->output.offset;

                    if(set_event_send(&ring,result.fd,connection->output.data+connection->output.offset,remaining,0)<0){
                        proactor_close_connection(result.fd);
                    }
                    continue;
                }
                if(ret<=0){
                    proactor_close_connection(result.fd);
                    continue;
                }
                connection->output.offset+=ret;
                if(connection->output.offset<connection->output.length){
                    int remaining=connection->output.length-connection->output.offset;

                    if(set_event_send(&ring,result.fd,connection->output.data+connection->output.offset,remaining,0)<0){
                        proactor_close_connection(result.fd);
                    }
                    continue;
                }
                connection->output.length=0;
                connection->output.offset=0;

                int stream_result=
                    proactor_schedule_stream_output(
                        &ring,
                        connection
                    );

                if(stream_result<0){
                   proactor_close_connection(result.fd);
                    continue;
                }

                /*
                * 已经提交下一块 SEND 或 TIMEOUT，
                * 不能再为同一连接提交 RECV。
                */
                if(stream_result>0){
                    continue;
                }

                int available=BUFFER_LENGTH-connection->input.length;
                if(available<=0){
                    proactor_close_connection(result.fd);
                    continue;
                }

                set_event_recv(&ring,result.fd,connection->input.data+connection->input.length,available,0);

            }else if(result.event==EVENT_STREAM_RETRY){
                /*
                * timeout 到期时，io_uring 通常返回 -ETIME。
                * 对 timeout 操作来说这是正常完成，不是连接错误。
                */
                if(entries->res<0 &&
                entries->res!=-ETIME){
                    continue;
                }

                if(result.fd<0 ||
                result.fd>=CONNECTION_SIZE){
                    continue;
                }

                struct conn *connection=
                    proactor_connections[result.fd];

                /*
                * 连接可能已经被其他完成事件关闭，
                * 此时忽略这个过期的 timeout。
                */
                if(connection==NULL){
                    continue;
                }

                int stream_result=
                    proactor_schedule_stream_output(
                        &ring,
                        connection
                    );

                if(stream_result<0){
                    proactor_close_connection(result.fd);
                    continue;
                }

                /*
                * stream_result > 0：
                * 已经提交 SEND，或者再次提交了 TIMEOUT。
                */
                if(stream_result>0){
                    continue;
                }

                /*
                * stream_result == 0：
                * 复制流已经结束，重新等待对端输入。
                */
                int available=
                    BUFFER_LENGTH-connection->input.length;

                if(available<=0 ||
                set_event_recv(
                        &ring,
                        result.fd,
                        connection->input.data+
                            connection->input.length,
                        available,
                        0)<0){

                    proactor_close_connection(result.fd);
                    continue;
                }
            }



        }
        io_uring_cq_advance(&ring,nready);



    }

}

/*
 * 保留旧的单监听器接口。
 * standalone 等旧调用仍然可以使用它。
 */
int proactor_start(
    unsigned short port,
    msg_handler handler)
{
    kvs_listener_config_t listener={
        .port=port,
        .handler=handler,
        .stream_handler=NULL
    };

    return proactor_run(
        &listener,
        1,
        NULL,
        0
    );
}

/*
 * 启动一个或多个监听器。
 * 所有监听器共用同一个 io_uring 完成事件循环。
 */
int proactor_start_listeners(
    const kvs_listener_config_t *listeners,
    size_t listener_count)
{
    if(listeners==NULL || listener_count==0){
        return -1;
    }

    return proactor_run(
        listeners,
        listener_count,
        NULL,
        0
    );
}

int proactor_start_runtime(
    const kvs_listener_config_t *listeners,
    size_t listener_count,
    const kvs_connector_config_t *connectors,
    size_t connector_count)
{
    if(listeners==NULL ||
       listener_count==0 ||
       (connector_count>0 && connectors==NULL)){
        return -1;
    }

    return proactor_run(
        listeners,
        listener_count,
        connectors,
        connector_count
    );
}