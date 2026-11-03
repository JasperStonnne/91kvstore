#ifndef KVS_RDMA_TRANSPORT_H
#define KVS_RDMA_TRANSPORT_H
#include <stddef.h>

/* 连接的具体资源由实现文件管理。 */
typedef struct kvs_rdma_connection kvs_rdma_connection_t;
/* Primary 用于接收 RDMA 连接请求的监听器。 */
typedef struct kvs_rdma_listener kvs_rdma_listener_t;

/* 传输模块向上层报告的连接事件。 */
typedef enum {
    KVS_RDMA_EVENT_ACCEPTING = 0,  /* 已提交接受操作，等待连接建立 */
    KVS_RDMA_EVENT_ESTABLISHED,    /* 连接已经建立 */
    KVS_RDMA_EVENT_DISCONNECTED   /* 连接已经断开 */
} kvs_rdma_event_type_t;

typedef struct {
    kvs_rdma_event_type_t type;
    kvs_rdma_connection_t *connection;
} kvs_rdma_event_t;

/* CQ 报告的数据收发完成类型，与前面的 CM 连接事件不同。 */
typedef enum {
    KVS_RDMA_COMPLETION_SEND,
    KVS_RDMA_COMPLETION_RECV
} kvs_rdma_completion_type_t;

typedef struct {
    kvs_rdma_completion_type_t type;

    /* 仅 RECV 有数据；指针借用内部缓冲区，不得 free。 */
    const void *data;
    size_t length;
} kvs_rdma_completion_t;



/* 创建空监听器对象；失败返回 NULL。 */
kvs_rdma_listener_t *kvs_rdma_listener_create(void);

/*
 * 创建 CM 资源、绑定地址并开始监听。
 * 返回 0 表示成功，-1 表示失败。
 * 失败时对象仍由调用方持有，需要关闭并释放。
 */
int kvs_rdma_listener_start(
    kvs_rdma_listener_t *listener,
    const char *bind_ip,
    unsigned short port,
    int backlog);

/*
 * 释放监听器及其内部资源。
 * 成功后将 *listener 置为 NULL；失败时保留对象，允许重试。
 * 调用前必须清理共用事件通道的连接，并确认已取出的 CM 事件。
 */
int kvs_rdma_listener_close(kvs_rdma_listener_t **listener);

/*
 * 获取 CM 事件通道 FD，供事件循环监听可读事件。
 * 返回的 FD 由监听器持有，调用方不得直接关闭或 read()。
 */
int kvs_rdma_listener_get_fd(const kvs_rdma_listener_t *listener);

/*
 * 释放连接及其资源，成功后将 *connection 置为 NULL。
 * 调用前必须停止并发访问，并确认已取出的相关 CM 事件。
 * 已建立的连接应先完成断开流程。
 * 失败时保留对象，允许继续清理。
 */
int kvs_rdma_connection_close(kvs_rdma_connection_t **connection);
/* 发起正常断开；成功后继续等待 CM 的 DISCONNECTED 事件。 */
int kvs_rdma_connection_disconnect(kvs_rdma_connection_t *connection);

/*
 * 非阻塞处理一个 CM 事件，当前管理一条连接。
 * *connection 初始为 NULL，创建连接后由调用方持有。
 *
 * 返回：
 *  1：生成事件通知，写入 event。
 *  0：暂无通知。
 * -1：处理失败，停止本次连接流程。
 *
 * 返回失败时，*connection 可能已创建，调用方仍需清理。
 * event->connection 也可能持有待清理对象；若与 *connection 不同，
 * 须另行关闭。额外连接请求被正常拒绝时返回 0，现有连接保留。
 */
int kvs_rdma_listener_poll(
    kvs_rdma_listener_t *listener,
    kvs_rdma_connection_t **connection,
    kvs_rdma_event_t *event,
    size_t capacity,
    int queue_depth);

/*
 * 复制数据到内部缓冲区，并提交发送。
 * 仅在连接建立后调用；同一连接不得并发操作。
 * 返回 0 表示提交成功，实际完成需检查 CQ。
 * 上一次发送未完成时返回 -1，errno 为 EBUSY。
 */
int kvs_rdma_connection_send(
    kvs_rdma_connection_t *connection,
    const void *data,
    size_t length);

/*
 * 非阻塞检查一次 CQ：
 * 1：取得完成通知；0：暂无通知；-1：失败，应停止该连接。
 * RECV 数据必须在 receive_next() 或关闭连接之前处理完。
 */
int kvs_rdma_connection_poll_completion(
    kvs_rdma_connection_t *connection,
    kvs_rdma_completion_t *completion);

/* 处理完当前接收数据后，投递下一次接收。 */
int kvs_rdma_connection_receive_next(
    kvs_rdma_connection_t *connection);

/*
 * 启动主动连接，*connection 必须初始为 NULL。
 * timeout_ms 同时限定整个主动建连过程（含地址/路由解析及等待 ESTABLISHED）。
 * 上层须持续 poll_event；到期返回 -1/ETIMEDOUT，成功建连后取消该期限。
 * 返回 0 表示地址解析请求提交成功，尚未建立连接。
 * 返回失败时也可能已创建对象，调用方需调用 close 清理。
 */
int kvs_rdma_connection_start(
    kvs_rdma_connection_t **connection,
    const char *host,
    unsigned short port,
    int timeout_ms);

/*
 * 非阻塞处理主动连接的一个 CM 事件。
 * 1：生成连接通知；0：暂无上层通知；-1：失败。
 * 地址、路由解析由函数内部推进。
 * 失败时连接对象仍由调用方持有并负责清理。
 */
int kvs_rdma_connection_poll_event(
    kvs_rdma_connection_t *connection,
    kvs_rdma_event_t *event,
    size_t capacity,
    int queue_depth);

#endif