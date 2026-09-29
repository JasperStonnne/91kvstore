//reactor.c
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/time.h>


#include "server.h"


#define MAX_PORTS			20

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

#if ENABLE_KVSTORE


int kvs_request(struct conn *c){
	if(c==NULL || c->handler==NULL){
			return -1;
	}

	int consumed_length=0;
	c->output.offset=0;
    c->output.length=c->handler(
                c->fd,
                c->input.data,
                c->input.length,
                c->output.data,
                BUFFER_LENGTH,
                &consumed_length
        );
	if(c->output.length<0){
		return -1;
	}
	if(kvs_input_buffer_consume(&c->input,consumed_length)<0){
		c->output.length=0;
		return -1;
	}
	return 0;
}

 int kvs_response(struct conn *c){
	(void)c;
	return 0;

}






#endif



int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);



int epfd = 0;
struct timeval begin;



struct conn conn_list[CONNECTION_SIZE] = {0};
// fd
/* ONLINE 状态下检查新 AOF 的间隔。 */
#define KVS_STREAM_RETRY_INTERVAL_MS 100

/* 当前等待新 AOF 的复制连接 fd。 */
static int stream_waiting_fd=-1;

/* 下一次检查新 AOF 的绝对时间，单位为毫秒。 */
static long long stream_retry_at_ms=-1;

/* 返回当前时间的毫秒值。 */
static long long reactor_now_ms(void)
{
        struct timeval now;

        if(gettimeofday(&now,NULL)!=0){
                return -1;
        }

        return (long long)now.tv_sec*1000+
               (long long)now.tv_usec/1000;
}

int set_event(int fd, int event, int flag) {

	struct epoll_event ev;
	ev.events = event;
	ev.data.fd=fd;
	if (flag){
		return epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&ev);
	}
	return epoll_ctl (epfd,EPOLL_CTL_MOD,fd,&ev);

}


int event_register(int fd, int event) {

if(fd<0 || fd>=CONNECTION_SIZE){
        return -1;
}

	conn_list[fd].fd = fd;
	conn_list[fd].r_action.recv_callback = recv_cb;
	conn_list[fd].send_callback = send_cb;

	memset(conn_list[fd].input.data, 0, BUFFER_LENGTH);
	conn_list[fd].input.length = 0;

	memset(conn_list[fd].output.data, 0, BUFFER_LENGTH);
	conn_list[fd].output.length = 0;
	conn_list[fd].output.offset=0;

/*
 * 将 epoll_ctl 的结果返回给调用者。
 * 注册失败时，调用者不能继续使用这条连接。
 */
return set_event(fd,event,1);
}

/*
 * 统一关闭一条已经注册到 Reactor 的连接。
 *
 * Reactor 只负责清理网络资源；
 * 如果业务层安装了 close_handler，再通过回调通知业务层。
 */
static void reactor_close_connection(int fd)
{
        /*
         * fd 必须位于 conn_list 范围内，
         * 并且该位置当前确实保存的是这条连接。
         */
        if(fd<0 ||
           fd>=CONNECTION_SIZE ||
           conn_list[fd].fd!=fd){
                return;
        }

        /*
         * 先保存回调。
         * 下面会清空 conn_list，避免同一个 fd 重复通知。
         */
        kvs_connection_close_handler close_handler=
                conn_list[fd].close_handler;

        /* 不再让 epoll 监控这条连接。 */
        epoll_ctl(
                epfd,
                EPOLL_CTL_DEL,
                fd,
                NULL
        );

        /* 关闭操作系统 socket。 */
        close(fd);

        /*
         * 如果关闭的正是等待新 AOF 的连接，
         * 同时取消它尚未到期的轮询闹钟。
         */
        if(stream_waiting_fd==fd){
                stream_waiting_fd=-1;
                stream_retry_at_ms=-1;
        }

        /* 清空这条连接在 Reactor 中保存的状态。 */
        conn_list[fd].fd=-1;
        conn_list[fd].handler=NULL;
        conn_list[fd].stream_handler=NULL;
        conn_list[fd].close_handler=NULL;
        conn_list[fd].input.length=0;
        conn_list[fd].output.length=0;
        conn_list[fd].output.offset=0;
        conn_list[fd].send_callback=NULL;
        conn_list[fd].r_action.recv_callback=NULL;

        /*
         * 最后通知业务层连接已经断开。
         * 普通客户端没有 close_handler，因此不会调用。
         */
        if(close_handler!=NULL){
                close_handler(fd);
        }
}

// listenfd(sockfd) --> EPOLLIN --> accept_cb
int accept_cb(int fd) {

	struct sockaddr_in  clientaddr;
	socklen_t len = sizeof(clientaddr);

	int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
	// /printf("accept finshed: %d,fd:%d\n", clientfd,fd);
	if (clientfd < 0) {
		printf("accept errno: %d --> %s\n", errno, strerror(errno));
		return -1;
	}

	event_register(clientfd, EPOLLIN);  // | EPOLLET
	/*
	* fd 是监听 socket，clientfd 是新建立的连接。
	* 新连接继承监听端口绑定的业务协议。
	*/
	conn_list[clientfd].handler=conn_list[fd].handler;
	conn_list[clientfd].stream_handler=conn_list[fd].stream_handler;
	conn_list[clientfd].close_handler=NULL;
	if ((clientfd % 1000) == 0) {

		struct timeval current;
		gettimeofday(&current, NULL);

		int time_used = TIME_SUB_MS(current, begin);
		memcpy(&begin, &current, sizeof(struct timeval));


		printf("accept finshed: %d, time_used: %d\n", clientfd, time_used);

	}

	return 0;
}


int recv_cb(int fd) {

	int available = BUFFER_LENGTH-conn_list[fd].input.length;
	if(available<=0){
		fprintf(stderr,"request buffer full: %d\n",fd);
		reactor_close_connection(fd);
		return 0;
	}
	int count = recv(fd,conn_list[fd].input.data+conn_list[fd].input.length,available,0);
	if (count == 0) { // disconnect
		printf("client disconnect: %d\n", fd);
		reactor_close_connection(fd);
		return 0;
	} else if (count < 0) { //

		printf("count: %d, errno: %d, %s\n", count, errno, strerror(errno));
		reactor_close_connection(fd);

		return 0;
	}


	conn_list[fd].input.length += count;
	//printf("RECV: %s\n", conn_list[fd].input.data);

#if ENABLE_HTTP

	http_request(&conn_list[fd]);

#elif ENABLE_WEBSOCKET

	ws_request(&conn_list[fd]);
#elif ENABLE_KVSTORE

	/*
 * 一次 recv 可能同时包含两个协议阶段的数据，例如：
 *
 * [Snapshot 最后一块][ONLINE 33\r\n]
 *
 * 每次处理后，只要：
 * 1. 没有响应等待发送；
 * 2. 输入缓冲区仍有数据；
 * 3. 本轮确实消费了数据；
 *
 * 就立即用新的复制状态继续处理剩余输入。
 */
while(1){
        int previous_input_length=
            conn_list[fd].input.length;

        if(kvs_request(&conn_list[fd])<0){
                reactor_close_connection(fd);
                return -1;
        }

        /*
         * 已经产生响应时先退出。
         * 响应必须由 send_cb() 发完，不能被下一轮覆盖。
         */
        if(conn_list[fd].output.length>0){
                break;
        }

        /* 输入已经全部处理完成。 */
        if(conn_list[fd].input.length==0){
                break;
        }

        /*
         * 输入长度没有减少，说明剩余数据还不是完整消息；
         * 等下一次 recv 补充，避免在这里无限循环。
         */
        if(conn_list[fd].input.length>=
           previous_input_length){
                break;
        }
}

	if(conn_list[fd].output.length==0){
		set_event(fd,EPOLLIN,0);
		return count;
	}

#endif


	set_event(fd, EPOLLOUT, 0);

	return count;
}

/*
 * 当 output 已经发送完时，向业务层索取下一块流数据。
 *
 * 返回值保持 stream_handler 的约定：
 * >0：output 中已经准备好下一块数据
 *  0：流结束，或者当前连接没有 stream_handler
 * -1：生成数据失败
 * -2：流未结束，但当前暂时没有新数据
 */
static int reactor_prepare_stream_output(struct conn *c)
{
        if(c==NULL){
                return -1;
        }

        if(c->stream_handler==NULL){
                return 0;
        }

        int stream_length=c->stream_handler(
                c->fd,
                c->output.data,
                BUFFER_LENGTH
        );

        if(stream_length>BUFFER_LENGTH){
                return -1;
        }

        if(stream_length>0){
                c->output.offset=0;
                c->output.length=stream_length;
        }

        return stream_length;
}
int send_cb(int fd) {

#if ENABLE_HTTP

	http_response(&conn_list[fd]);

#elif ENABLE_WEBSOCKET

	ws_response(&conn_list[fd]);

#elif ENABLE_KVSTORE
	kvs_response(&conn_list[fd]);

#endif
	int count = 0;



	int remaining=conn_list[fd].output.length-conn_list[fd].output.offset;
	if(remaining>0){
		count=send(fd,conn_list[fd].output.data+conn_list[fd].output.offset,remaining,0);
	}
	if(count<0){
		if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR){
			set_event(fd,EPOLLOUT,0);
			return 0;
		}
		reactor_close_connection(fd);
		return -1;
	}
	if(count>0){
		conn_list[fd].output.offset+=count;//累加本次实际发送的字节数
		 if(conn_list[fd].output.offset<conn_list[fd].output.length){
			set_event(fd,EPOLLOUT,0);
			return count;
		 }
		 conn_list[fd].output.length =0;
		 conn_list[fd].output.offset=0;
	}

        int stream_length=reactor_prepare_stream_output(
                &conn_list[fd]
        );

        if(stream_length>0){
                /*
                 * 下一块 Snapshot/AOF 已经放进 output，
                 * 等 socket 可写时再次进入 send_cb()。
                 */
                set_event(fd,EPOLLOUT,0);
                return count;
        }

        if(stream_length==KVS_STREAM_WAIT){
			long long now_ms=reactor_now_ms();
        if(now_ms<0){
            reactor_close_connection(fd);
            return -1;
        }

        /*
         * 记录需要重新检查的连接，
         * 并把检查时间设置为 100ms 后。
         */
        stream_waiting_fd=fd;
        stream_retry_at_ms=
                now_ms+KVS_STREAM_RETRY_INTERVAL_MS;

        set_event(fd,EPOLLIN,0);
        return count;
        }

        if(stream_length<0){
			reactor_close_connection(fd);
        }

	set_event(fd, EPOLLIN, 0);

	return count;
}



int r_init_server(unsigned short port) {

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

/*
 * 根据 connector 配置主动连接远端服务器。
 *
 * 成功：返回已经连接的 socket fd。
 * 失败：返回 -1。
 *
 * 这里只建立 TCP 连接，还不注册进 epoll。
 */
static int reactor_open_connector_socket(
    const kvs_connector_config_t *connector)
{
        if(connector==NULL ||
           connector->host==NULL ||
           connector->host[0]=='\0' ||
           connector->port==0){
                return -1;
        }

        int fd=socket(AF_INET,SOCK_STREAM,0);
        if(fd<0){
                return -1;
        }

        struct sockaddr_in remote_address={0};
        remote_address.sin_family=AF_INET;
        remote_address.sin_port=htons(connector->port);

        /*
         * 把 "127.0.0.1" 这样的字符串地址，
         * 转换成 sockaddr_in 使用的二进制地址。
         */
        if(inet_pton(
                AF_INET,
                connector->host,
                &remote_address.sin_addr)!=1){

                close(fd);
                return -1;
        }

        if(connect(
                fd,
                (struct sockaddr *)&remote_address,
                sizeof(remote_address))!=0){

                close(fd);
                return -1;
        }

        return fd;
}

static int reactor_run(
    const kvs_listener_config_t *listeners,
    size_t listener_count,
    const kvs_connector_config_t *connectors,
    size_t connector_count)
{

	epfd = epoll_create(1);

for(size_t i=0;i<listener_count;i++){
        int sockfd=r_init_server(listeners[i].port);
        if(sockfd<0){
                return -1;
        }

        conn_list[sockfd].fd=sockfd;
        conn_list[sockfd].handler=listeners[i].handler;
		conn_list[sockfd].stream_handler=listeners[i].stream_handler;
        conn_list[sockfd].r_action.recv_callback=accept_cb;

        set_event(sockfd,EPOLLIN,1);

        printf("listen port : %u\n",listeners[i].port);
}

/*
 * 主动建立并注册 connectors 中的连接。
 */
for(size_t i=0;i<connector_count;i++){
        int fd=reactor_open_connector_socket(
            &connectors[i]
        );

        if(fd<0){
                return -1;
        }

        /*
         * conn_list 直接使用 fd 作为数组下标，
         * 所以必须先保证它没有越界。
         */
        if(fd>=CONNECTION_SIZE){
                close(fd);
                return -1;
        }

        /*
         * 将主动连接交给 epoll。
         * 目前先监听 Primary 发来的数据。
         */
        if(event_register(fd,EPOLLIN)<0){
                reactor_close_connection(fd);
                return -1;
        }

        /*
         * 收到 Primary 数据时，交给连接器提供的消息处理函数。
         * Replica 的这个函数实际来自 replication.c。
         */
        conn_list[fd].handler=
            connectors[i].message_handler;

        /*
         * Replica 的上游连接只接收 Primary 的复制流，
         * 不通过 stream_handler 主动产生 Snapshot/AOF。
         */
        conn_list[fd].stream_handler=NULL;

        /*
         * 连接断开时通知 replication.c 清理复制状态。
         */
        conn_list[fd].close_handler=
            connectors[i].close_handler;

		/*
		* 通知业务层：到 Primary 的连接已经建立。
		*
		* Replica 的 open_handler 会生成第一条握手消息 PING，
		* 并把它写入当前连接的 output 缓冲区。
		*/
		int output_length=connectors[i].open_handler(
			fd,
			conn_list[fd].output.data,
			BUFFER_LENGTH
		);

		if(output_length<0 ||
		output_length>BUFFER_LENGTH){
				reactor_close_connection(fd);
				return -1;
		}

		conn_list[fd].output.offset=0;
		conn_list[fd].output.length=output_length;

		/*
		* output 中存在 PING 时，改为关注 EPOLLOUT。
		* socket 可以写时，统一由现有 send_cb() 发送。
		*/
		if(output_length>0){
				if(set_event(fd,EPOLLOUT,0)<0){
						reactor_close_connection(fd);
						return -1;
				}
		}

        printf(
            "connect to primary %s:%u\n",
            connectors[i].host,
            connectors[i].port
        );
}


	gettimeofday(&begin, NULL);

	while (1) { // mainloop

		struct epoll_event events[1024] = {0};
		int wait_timeout=-1;
	if(stream_waiting_fd>=0){
			long long now_ms=reactor_now_ms();
			if(now_ms<0){
					return -1;
			}

			/*
			* 计算距离复制连接下一次检查还剩多少毫秒。
			*
			* 普通客户端事件可能让 epoll_wait 提前返回，
			* 因此不能每次都重新等待完整的 100ms。
			*/
			long long remaining_ms=
					stream_retry_at_ms-now_ms;

			/*
			* 时间还没到：等待剩余时间。
			* 时间已经到：传入 0，让 epoll_wait 立即返回。
			*/
			wait_timeout=remaining_ms>0 ? (int)remaining_ms : 0;
	}

		int nready = epoll_wait(epfd, events, 1024,wait_timeout);
		if(nready<0){
        /*
         * epoll_wait 被信号临时中断，不是网络故障，
         * 重新进入主循环继续等待即可。
         */
        if(errno==EINTR){
                continue;
        }

        /* 其他错误无法继续运行 Reactor。 */
        return -1;
}
		int i = 0;
		for (i = 0;i < nready;i ++) {

			int connfd = events[i].data.fd;

#if 0
			if (events[i].events & EPOLLIN) {
				conn_list[connfd].r_action.recv_callback(connfd);
			} else if (events[i].events & EPOLLOUT) {
				conn_list[connfd].send_callback(connfd);
			}

#else
			if (events[i].events & EPOLLIN) {
				conn_list[connfd].r_action.recv_callback(connfd);
			}

			if (events[i].events & EPOLLOUT) {
				conn_list[connfd].send_callback(connfd);
			}
#endif
		}
/*
 * stream_waiting_fd >= 0 表示有一条复制连接正在等待新 AOF。
 *
 * epoll_wait 可能因为两种原因返回：
 * 1. 收到了普通网络事件；
 * 2. 等待时间达到 100ms。
 *
 * 因此不能在 epoll_wait 返回后立刻唤醒复制连接，
 * 还需要检查预定的重试时间是否真的已经到达。
 */
			if(stream_waiting_fd>=0){
					long long now_ms=reactor_now_ms();
					if(now_ms<0){
							return -1;
					}

					/*
					* 当前时间已经达到预定时间，
					* 可以让复制连接再次检查 Primary AOF。
					*/
					if(now_ms>=stream_retry_at_ms){
							/*
							* 先保存 fd，因为下面要把全局等待状态清空。
							*/
							int waiting_fd=stream_waiting_fd;

							/*
							* 表示当前不再有等待唤醒的连接。
							*
							* 如果 send_cb 再次发现没有新 AOF，
							* 它会重新写入 fd 和下一次重试时间。
							*/
							stream_waiting_fd=-1;
							stream_retry_at_ms=-1;

							/*
							* 关注 waiting_fd 的可写事件。
							*
							* 下一轮 epoll_wait 发现 socket 可以写时，
							* 会调用 send_cb()，进而再次调用
							* stream_handler 检查有没有新的 AOF 数据。
							*/
							set_event(
									waiting_fd,
									EPOLLOUT,
									0
							);
        }
}

	}
    return 0;

}

int reactor_start_listeners(
    const kvs_listener_config_t *listeners,
    size_t listener_count)
{
       if(listeners==NULL || listener_count==0){
        return -1;
    }

    for(size_t i=0;i<listener_count;i++){
        if(listeners[i].port==0 ||
           listeners[i].handler==NULL){
            return -1;
        }
    }

return reactor_run(
    listeners,
    listener_count,
	NULL,
	0
);
}

int reactor_start_runtime(
    const kvs_listener_config_t *listeners,
    size_t listener_count,
    const kvs_connector_config_t *connectors,
    size_t connector_count)
{
        /*
         * 当前至少需要一个监听器。
         * Replica 要用它监听客户端服务端口。
         */
        if(listeners==NULL || listener_count==0){
                return -1;
        }


        /*
         * connector_count 大于 0 时，
         * 调用者必须提供对应的连接器配置数组。
         */
        if(connector_count>0 && connectors==NULL){
                return -1;
        }
		for(size_t i=0;i<connector_count;i++){
        if(connectors[i].host==NULL ||
           connectors[i].host[0]=='\0' ||
           connectors[i].port==0 ||
           connectors[i].open_handler==NULL ||
           connectors[i].message_handler==NULL){
                return -1;
        }
}
        return reactor_run(
            listeners,
            listener_count,
            connectors,
            connector_count
        );
}
/*
 * 保留原来的单监听器启动接口。
 * 内部统一转成新的 listener 配置，避免维护两套事件循环。
 */
int reactor_start(
    unsigned short port,
    msg_handler handler)
{
        kvs_listener_config_t listener={
            .port=port,
            .handler=handler,
            .stream_handler=NULL
        };

        return reactor_start_listeners(
            &listener,
            1
        );
}