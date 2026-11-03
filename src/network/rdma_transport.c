#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <time.h>
#include "rdma_transport.h"

/* 一组独立的 RDMA 收发缓冲区及其内存注册信息。 */
typedef struct {
    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;

    char *rbuffer;
    char *sbuffer;

    size_t capacity;
} kvs_rdma_buffers_t;


/*
 * Primary 的 RDMA 监听资源。

 * event_channel 接收 CM 连接管理事件。
 * listen_id 用于绑定地址和端口、监听连接请求。
 */
struct kvs_rdma_listener {
    struct rdma_event_channel *event_channel;
    struct rdma_cm_id *listen_id;
};

/* 对应头文件中声明的不透明连接类型。 */
struct kvs_rdma_connection {
    struct rdma_cm_id *cm_id;
    /* 仅保存本连接自己创建、需要自己释放的 CM 通道。 */
    struct rdma_event_channel *owned_cm_channel;
    /* 地址和路由解析使用的超时，单位毫秒。 */
    int resolve_timeout_ms;
    /* 主动建连的单调时钟截止时间；ESTABLISHED 后清零，不限制数据传输时间。 */
    int64_t connect_deadline_ms;
    /* 已取出但尚未确认的 CM 事件，由该对象持有。 */
    struct rdma_cm_event *pending_cm_event;
    struct ibv_pd *pd;
    struct ibv_cq *cq;

    kvs_rdma_buffers_t buffers;
    /* 每个方向暂时只允许一个未完成的工作请求。 */
    int send_pending;
    int recv_pending;
};

/*
 * 调用前必须确保没有工作请求继续引用这些缓冲区。
 * 传入对象由调用方持有，本函数只释放其内部资源。
 */
static int kvs_rdma_buffers_destroy(kvs_rdma_buffers_t *buffers)
{
    if (buffers == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (buffers->send_mr != NULL) {
        int error = ibv_dereg_mr(buffers->send_mr);
        if (error != 0) {
            errno = error;
            return -1;
        }
        buffers->send_mr = NULL;
    }

    if (buffers->recv_mr != NULL) {
        int error = ibv_dereg_mr(buffers->recv_mr);
        if (error != 0) {
            errno = error;
            return -1;
        }
        buffers->recv_mr = NULL;
    }

    free(buffers->rbuffer);
    free(buffers->sbuffer);
    memset(buffers, 0, sizeof(*buffers));
    return 0;
}

/* buffers 必须以 {0} 初始化，capacity 是每块缓冲区的容量。 */
static int kvs_rdma_buffers_init(
    kvs_rdma_buffers_t *buffers,
    struct ibv_pd *pd,
    size_t capacity)
{
    if (buffers == NULL || pd == NULL ||
        capacity == 0 || capacity > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    if (buffers->rbuffer != NULL || buffers->sbuffer != NULL ||
        buffers->recv_mr != NULL || buffers->send_mr != NULL ||
        buffers->capacity != 0) {
        errno = EALREADY;
        return -1;
    }

    buffers->capacity = capacity;
    buffers->rbuffer = calloc(1, capacity);
    buffers->sbuffer = calloc(1, capacity);

    if (buffers->rbuffer == NULL || buffers->sbuffer == NULL) {
        goto fail;
    }

    buffers->send_mr = ibv_reg_mr(
        pd, buffers->sbuffer, capacity, 0);
    if (buffers->send_mr == NULL) {
        goto fail;
    }

    buffers->recv_mr = ibv_reg_mr(
        pd, buffers->rbuffer, capacity, IBV_ACCESS_LOCAL_WRITE);
    if (buffers->recv_mr == NULL) {
        goto fail;
    }

    return 0;

fail:
    {
        int saved_errno = errno;

        if (kvs_rdma_buffers_destroy(buffers) < 0) {
            return -1;
        }

        errno = saved_errno;
        return -1;
    }
}

/*
 * cm_id 必须已经关联本机 RDMA 设备，
 * 即 cm_id->verbs 非空。
 */
static int kvs_rdma_queuepair_init(
    kvs_rdma_connection_t *connection,
    int queue_depth)
{
    if (connection == NULL ||
        connection->cm_id == NULL ||
        connection->cm_id->verbs == NULL ||
        queue_depth <= 0 || queue_depth > INT32_MAX / 2) {
        errno = EINVAL;
        return -1;
    }

    if (connection->pd != NULL ||
        connection->cq != NULL ||
        connection->cm_id->qp != NULL) {
        errno = EALREADY;
        return -1;
    }

    connection->pd = ibv_alloc_pd(connection->cm_id->verbs);
    if (connection->pd == NULL) {
        return -1;
    }

    /* 同一个 CQ 接收发送和接收两个方向的完成记录。 */
    connection->cq = ibv_create_cq(
        connection->cm_id->verbs,
        queue_depth * 2,
        NULL, NULL, 0);
    if (connection->cq == NULL) {
        goto fail;
    }

    struct ibv_qp_init_attr attr = {0};
    attr.send_cq = connection->cq;
    attr.recv_cq = connection->cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = (uint32_t)queue_depth;
    attr.cap.max_recv_wr = (uint32_t)queue_depth;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(
            connection->cm_id, connection->pd, &attr) != 0) {
        goto fail;
    }

    return 0;

fail:
    {
        int saved_errno = errno;

        if (connection->cq != NULL) {
            int error = ibv_destroy_cq(connection->cq);
            if (error != 0) {
                errno = error;
                return -1;
            }
            connection->cq = NULL;
        }

        int error = ibv_dealloc_pd(connection->pd);
        if (error != 0) {
            errno = error;
            return -1;
        }
        connection->pd = NULL;

        errno = saved_errno;
        return -1;
    }
}

static int kvs_rdma_post_recv(kvs_rdma_connection_t *connection)
{
    if (connection == NULL ||
        connection->cm_id == NULL ||
        connection->cm_id->qp == NULL ||
        connection->buffers.recv_mr == NULL ||
        connection->buffers.rbuffer == NULL ||
        connection->buffers.capacity == 0 ||
        connection->buffers.capacity > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    if (connection->recv_pending) {
        errno = EBUSY;
        return -1;
    }

    struct ibv_sge sge = {0};
    sge.addr = (uintptr_t)connection->buffers.rbuffer;
    sge.length = (uint32_t)connection->buffers.capacity;
    sge.lkey = connection->buffers.recv_mr->lkey;

    struct ibv_recv_wr wr = {0};
    struct ibv_recv_wr *bad = NULL;

    wr.wr_id = (uintptr_t)connection;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    int error = ibv_post_recv(connection->cm_id->qp, &wr, &bad);
    if (error != 0) {
        errno = error;
        return -1;
    }

    connection->recv_pending = 1;
    return 0;
}

/* 调用前，sbuffer 中应已准备好 length 字节的数据。 */
static int kvs_rdma_post_send(
    kvs_rdma_connection_t *connection,
    size_t length)
{
    if (connection == NULL ||
        connection->cm_id == NULL ||
        connection->cm_id->qp == NULL ||
        connection->buffers.send_mr == NULL ||
        connection->buffers.sbuffer == NULL ||
        length == 0 ||
        length > connection->buffers.capacity ||
        length > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    if (connection->send_pending) {
        errno = EBUSY;
        return -1;
    }

    struct ibv_sge sge = {0};
    sge.addr = (uintptr_t)connection->buffers.sbuffer;
    sge.length = (uint32_t)length;
    sge.lkey = connection->buffers.send_mr->lkey;

    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad = NULL;

    wr.wr_id = (uintptr_t)connection;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    int error = ibv_post_send(connection->cm_id->qp, &wr, &bad);
    if (error != 0) {
        errno = error;
        return -1;
    }

    connection->send_pending = 1;
    return 0;
}

static int kvs_rdma_poll_completion(
    kvs_rdma_connection_t *connection,
    struct ibv_wc *completion)
{
    if (connection == NULL ||
        connection->cq == NULL ||
        completion == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct ibv_wc wc = {0};
    int count = ibv_poll_cq(connection->cq, 1, &wc);

    if (count < 0) {
        errno = EIO;
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    *completion = wc;

    /* 失败完成不能作为缓冲区正常可复用的依据。 */
    if (wc.status != IBV_WC_SUCCESS) {
        errno = EIO;
        return -1;
    }

    if (wc.wr_id != (uintptr_t)connection) {
        errno = EPROTO;
        return -1;
    }

    switch (wc.opcode) {
    case IBV_WC_SEND:
        if (!connection->send_pending) {
            errno = EPROTO;
            return -1;
        }
        connection->send_pending = 0;
        break;

    case IBV_WC_RECV:
        if (!connection->recv_pending ||
            wc.byte_len > connection->buffers.capacity) {
            errno = EPROTO;
            return -1;
        }
        connection->recv_pending = 0;
        break;

    default:
        errno = EPROTO;
        return -1;
    }

    return 1;
}

/*
 * 调用前，上层必须停止投递和对该连接的并发访问。
 * 这里只清理本模块创建的 QP、MR、缓冲区、CQ、PD。
 * CM ID 和事件通道稍后由连接管理逻辑清理。
 */
static int kvs_rdma_resources_destroy(
    kvs_rdma_connection_t *connection)
{
    if (connection == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 先销毁 QP，成功后才允许释放其使用的缓冲区。 */
    if (connection->cm_id != NULL &&
        connection->cm_id->qp != NULL) {
        int error = ibv_destroy_qp(connection->cm_id->qp);
        if (error != 0) {
            errno = error;
            return -1;
        }
        connection->cm_id->qp = NULL;
    }

    connection->send_pending = 0;
    connection->recv_pending = 0;

    if (kvs_rdma_buffers_destroy(&connection->buffers) < 0) {
        return -1;
    }

    if (connection->cq != NULL) {
        int error = ibv_destroy_cq(connection->cq);
        if (error != 0) {
            errno = error;
            return -1;
        }
        connection->cq = NULL;
    }

    if (connection->pd != NULL) {
        int error = ibv_dealloc_pd(connection->pd);
        if (error != 0) {
            errno = error;
            return -1;
        }
        connection->pd = NULL;
    }

    return 0;
}

/*
 * 创建监听器的 CM 资源，尚未绑定地址或开始监听。
 * listener 必须以 {0} 初始化。
 */
static int kvs_rdma_listener_init(kvs_rdma_listener_t *listener)
{
    if (listener == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (listener->event_channel != NULL ||
        listener->listen_id != NULL) {
        errno = EALREADY;
        return -1;
    }

    listener->event_channel = rdma_create_event_channel();
    if (listener->event_channel == NULL) {
        return -1;
    }
    /* 没有 CM 事件时立即返回，避免阻塞服务线程。 */
    int flags = fcntl(listener->event_channel->fd, F_GETFL);
    if (flags < 0 ||
        fcntl(listener->event_channel->fd,
            F_SETFL, flags | O_NONBLOCK) < 0) {
        int saved_errno = errno;

        rdma_destroy_event_channel(listener->event_channel);
        listener->event_channel = NULL;

        errno = saved_errno;
        return -1;
    }
    if (rdma_create_id(
            listener->event_channel,
            &listener->listen_id,
            listener,
            RDMA_PS_TCP) != 0) {
        int saved_errno = errno;

        rdma_destroy_event_channel(listener->event_channel);
        listener->event_channel = NULL;

        errno = saved_errno;
        return -1;
    }

    return 0;
}

/*
 * 将监听器绑定到明确的本机 IPv4 地址和固定端口。
 * 调用前必须成功执行 kvs_rdma_listener_init()。
 */
static int kvs_rdma_listener_bind(
    kvs_rdma_listener_t *listener,
    const char *bind_ip,
    unsigned short port)
{
    if (listener == NULL ||
        listener->event_channel == NULL ||
        listener->listen_id == NULL ||
        bind_ip == NULL ||
        port == 0) {
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    int result = inet_pton(AF_INET, bind_ip, &address.sin_addr);
    if (result != 1) {
        if (result == 0) {
            errno = EINVAL;
        }
        return -1;
    }

    if (rdma_bind_addr(
            listener->listen_id,
            (struct sockaddr *)&address) != 0) {
        return -1;
    }

    /* 绑定明确的本机 IP 后，应当已经关联对应的 RDMA 设备。 */
    struct ibv_context *verbs = listener->listen_id->verbs;
    if (verbs == NULL) {
        errno = ENODEV;
        return -1;
    }

    /* 本项目的 RDMA 全量同步要求使用 iWARP。 */
    if (verbs->device->transport_type != IBV_TRANSPORT_IWARP) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    return 0;
}

/*
 * 开始接收 RDMA 连接请求。
 * 调用前必须成功完成 init 和 bind。
 */
static int kvs_rdma_listener_listen(
    kvs_rdma_listener_t *listener,
    int backlog)
{
    if (listener == NULL ||
        listener->event_channel == NULL ||
        listener->listen_id == NULL ||
        backlog <= 0) {
        errno = EINVAL;
        return -1;
    }

    return rdma_listen(listener->listen_id, backlog);
}

/*
 * 释放监听器的 CM 资源。
 * 调用前必须停止并发访问，确认已取出的 CM 事件均已确认，
 * 并销毁所有共用该事件通道的连接 ID。
 */
static int kvs_rdma_listener_destroy(kvs_rdma_listener_t *listener)
{
    if (listener == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (listener->listen_id != NULL) {
        if (rdma_destroy_id(listener->listen_id) != 0) {
            return -1;
        }
        listener->listen_id = NULL;
    }

    if (listener->event_channel != NULL) {
        rdma_destroy_event_channel(listener->event_channel);
        listener->event_channel = NULL;
    }

    return 0;
}

/*
 * 尝试获取一个 CM 事件：
 *  1：取得事件，调用方处理后必须调用 rdma_ack_cm_event()。
 *  0：暂时没有事件，或本次调用被信号中断，稍后重试。
 * -1：获取失败。
 *
 * 返回 1 只表示取得事件，事件本身是否成功由调用方检查。
 */
static int kvs_rdma_poll_cm_event(
    struct rdma_event_channel *channel,
    struct rdma_cm_event **event)
{
    if (channel == NULL || event == NULL) {
        errno = EINVAL;
        return -1;
    }

    *event = NULL;

    /* 零超时检查：没有事件就立即返回，不进入底层读取。 */
    struct pollfd descriptor = {
        .fd = channel->fd,
        .events = POLLIN
    };

    int ready = poll(&descriptor, 1, 0);
    if (ready < 0) {
        return errno == EINTR ? 0 : -1;
    }

    if (ready == 0) {
        return 0;
    }

    if (!(descriptor.revents & POLLIN)) {
        errno = EIO;
        return -1;
    }

    /* 通道可读后，仍由 librdmacm 正式取出事件。 */
    if (rdma_get_cm_event(channel, event) == 0) {
        return 1;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK ||
        errno == EINTR) {
        return 0;
    }

    return -1;
}

kvs_rdma_listener_t *kvs_rdma_listener_create(void)
{
    return calloc(1, sizeof(kvs_rdma_listener_t));
}

int kvs_rdma_listener_start(
    kvs_rdma_listener_t *listener,
    const char *bind_ip,
    unsigned short port,
    int backlog)
{
    if (listener == NULL || bind_ip == NULL ||
        port == 0 || backlog <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (kvs_rdma_listener_init(listener) < 0) {
        return -1;
    }

    if (kvs_rdma_listener_bind(listener, bind_ip, port) < 0) {
        return -1;
    }

    return kvs_rdma_listener_listen(listener, backlog);
}

int kvs_rdma_listener_close(kvs_rdma_listener_t **listener)
{
    if (listener == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (*listener == NULL) {
        return 0;
    }

    if (kvs_rdma_listener_destroy(*listener) < 0) {
        return -1;
    }

    free(*listener);
    *listener = NULL;
    return 0;
}

int kvs_rdma_listener_get_fd(const kvs_rdma_listener_t *listener)
{
    if (listener == NULL || listener->event_channel == NULL) {
        errno = EINVAL;
        return -1;
    }

    return listener->event_channel->fd;
}


/*
 * 为新连接准备收发资源。
 * 失败时可能已有部分资源创建成功，调用方必须清理连接。
 */
static int kvs_rdma_connection_prepare(
    kvs_rdma_connection_t *connection,
    size_t capacity,
    int queue_depth)
{
    if (connection == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (kvs_rdma_queuepair_init(connection, queue_depth) < 0) {
        return -1;
    }

    if (kvs_rdma_buffers_init(
            &connection->buffers,
            connection->pd,
            capacity) < 0) {
        return -1;
    }

    return kvs_rdma_post_recv(connection);


}

int kvs_rdma_connection_disconnect(
    kvs_rdma_connection_t *connection)
{
    if (connection == NULL ||
        connection->cm_id == NULL ||
        connection->cm_id->qp == NULL) {
        errno = EINVAL;
        return -1;
    }

    return rdma_disconnect(connection->cm_id);
}

int kvs_rdma_connection_close(kvs_rdma_connection_t **connection)
{
    if (connection == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (*connection == NULL) {
        return 0;
    }

    kvs_rdma_connection_t *current = *connection;

    /* ACK 失败时保留的事件，在关闭 ID 前重试确认。 */
    if (current->pending_cm_event != NULL) {
        if (rdma_ack_cm_event(current->pending_cm_event) != 0) {
            return -1;
        }
        current->pending_cm_event = NULL;
    }

    if (kvs_rdma_resources_destroy(current) < 0) {
        return -1;
    }

    if (current->cm_id != NULL) {
        if (rdma_destroy_id(current->cm_id) != 0) {
            return -1;
        }
        current->cm_id = NULL;
    }
    if (current->owned_cm_channel != NULL) {
    rdma_destroy_event_channel(current->owned_cm_channel);
    current->owned_cm_channel = NULL;
    }
    free(current);
    *connection = NULL;
    return 0;
}

/*
 * 接受一个已完成资源准备的连接请求。
 * 返回 0 仅表示接受操作提交成功，
 * 仍需等待 CM 的 ESTABLISHED 事件。
 */
static int kvs_rdma_connection_accept(
    kvs_rdma_connection_t *connection)
{
    if (connection == NULL ||
        connection->cm_id == NULL ||
        connection->cm_id->qp == NULL ||
        !connection->recv_pending) {
        errno = EINVAL;
        return -1;
    }

    struct rdma_conn_param parameters = {0};

    return rdma_accept(connection->cm_id, &parameters);
}

/*
 * 使用外层已经创建的连接对象，
 * 准备收发资源并接受连接请求。
 * 事件确认和失败清理由外层负责。
 */
static int kvs_rdma_handle_connect_request(
    kvs_rdma_listener_t *listener,
    struct rdma_cm_event *event,
    kvs_rdma_connection_t *connection,
    size_t capacity,
    int queue_depth)
{
    if (listener == NULL || listener->listen_id == NULL ||
        event == NULL || connection == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST ||
        event->listen_id != listener->listen_id ||
        event->id == NULL ||
        connection->cm_id != event->id ||
        event->status != 0) {
        errno = EPROTO;
        return -1;
    }

    if (kvs_rdma_connection_prepare(
            connection, capacity, queue_depth) < 0) {
        return -1;
    }

    return kvs_rdma_connection_accept(connection);
}

/*
 * 转换已有连接的 CM 事件：
 *  1：生成了一条上层通知。
 *  0：事件无需通知上层。
 * -1：事件异常。
 *
 * 本函数不确认 CM 事件，也不释放连接。
 */
static int kvs_rdma_handle_connection_event(
    kvs_rdma_connection_t *connection,
    const struct rdma_cm_event *cm_event,
    kvs_rdma_event_t *output,
    size_t capacity,
    int queue_depth)
{
    if (connection == NULL || connection->cm_id == NULL ||
        cm_event == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (cm_event->id != connection->cm_id) {
        errno = EPROTO;
        return -1;
    }

    if (cm_event->status != 0) {
        errno = EIO;
        return -1;
    }

    kvs_rdma_event_type_t type;

    switch (cm_event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        /* 此阶段只应出现在主动连接的一侧。 */
        if (connection->owned_cm_channel == NULL ||
            connection->resolve_timeout_ms <= 0) {
            errno = EPROTO;
            return -1;
        }

        if (connection->cm_id->verbs == NULL) {
            errno = ENODEV;
            return -1;
        }

        if (connection->cm_id->verbs->device->transport_type !=
            IBV_TRANSPORT_IWARP) {
            errno = EPROTONOSUPPORT;
            return -1;
        }

        return rdma_resolve_route(
            connection->cm_id,
            connection->resolve_timeout_ms);

    case RDMA_CM_EVENT_ROUTE_RESOLVED: {
        if (connection->owned_cm_channel == NULL) {
            errno = EPROTO;
            return -1;
        }

        if (kvs_rdma_connection_prepare(
                connection, capacity, queue_depth) < 0) {
            return -1;
        }

        struct rdma_conn_param parameters = {0};

        return rdma_connect(connection->cm_id, &parameters);
        }
    case RDMA_CM_EVENT_ESTABLISHED:
        connection->connect_deadline_ms = 0;
        type = KVS_RDMA_EVENT_ESTABLISHED;
        break;

    case RDMA_CM_EVENT_DISCONNECTED:
        type = KVS_RDMA_EVENT_DISCONNECTED;
        break;

    case RDMA_CM_EVENT_TIMEWAIT_EXIT:
        return 0;

    default:
        errno = EPROTO;
        return -1;
    }

    output->type = type;
    output->connection = connection;
    return 1;
}

/*
 * 分发 CM 事件：
 *  1：生成上层通知。
 *  0：无需通知上层。
 * -1：处理失败。
 *
 * 事件确认和失败清理由外层负责。
 */
static int kvs_rdma_dispatch_cm_event(
    kvs_rdma_listener_t *listener,
    struct rdma_cm_event *cm_event,
    kvs_rdma_connection_t **connection,
    kvs_rdma_event_t *output,
    size_t capacity,
    int queue_depth)
{
    if (listener == NULL || cm_event == NULL ||
        connection == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (cm_event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
        if (kvs_rdma_handle_connect_request(
                listener, cm_event, *connection,
                capacity, queue_depth) < 0) {
            return -1;
        }

        output->type = KVS_RDMA_EVENT_ACCEPTING;
        output->connection = *connection;
        return 1;
    }

    return kvs_rdma_handle_connection_event(
    *connection, cm_event, output, capacity, queue_depth);
}

/*
 * 确认事件已用完。
 * 成功后事件对象已失效，将调用方的指针清空。
 * 失败时保留指针。
 */
static int kvs_rdma_ack_cm_event(struct rdma_cm_event **event)
{
    if (event == NULL || *event == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (rdma_ack_cm_event(*event) != 0) {
        return -1;
    }

    *event = NULL;
    return 0;
}

int kvs_rdma_connection_send(
    kvs_rdma_connection_t *connection,
    const void *data,
    size_t length)
{
    if (connection == NULL || data == NULL ||
        connection->buffers.sbuffer == NULL ||
        length == 0 ||
        length > connection->buffers.capacity ||
        length > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    /* 必须在复制前检查，避免覆盖尚未发送完的数据。 */
    if (connection->send_pending) {
        errno = EBUSY;
        return -1;
    }

    memcpy(connection->buffers.sbuffer, data, length);

    return kvs_rdma_post_send(connection, length);
}

int kvs_rdma_connection_poll_completion(
    kvs_rdma_connection_t *connection,
    kvs_rdma_completion_t *completion)
{
    if (completion == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct ibv_wc wc = {0};
    int result = kvs_rdma_poll_completion(connection, &wc);
    if (result <= 0) {
        return result;
    }

    kvs_rdma_completion_t notification = {0};

    if (wc.opcode == IBV_WC_SEND) {
        notification.type = KVS_RDMA_COMPLETION_SEND;
    } else {
        /* 内部函数已检查，只允许成功的 SEND 或 RECV。 */
        notification.type = KVS_RDMA_COMPLETION_RECV;
        notification.data = connection->buffers.rbuffer;
        notification.length = wc.byte_len;
    }

    *completion = notification;
    return 1;
}

int kvs_rdma_connection_receive_next(
    kvs_rdma_connection_t *connection)
{
    return kvs_rdma_post_recv(connection);
}

/*
 * 处理一个 CM 事件。先准备资源持有者，再取事件；
 * 因此即使后续准备连接失败，新 CM ID 也不会失去清理入口。
 */
int kvs_rdma_listener_poll(
    kvs_rdma_listener_t *listener,
    kvs_rdma_connection_t **connection,
    kvs_rdma_event_t *event,
    size_t capacity,
    int queue_depth)
{
    if (event != NULL) {
        *event = (kvs_rdma_event_t){0};
    }
    if (listener == NULL || listener->listen_id == NULL ||
        listener->event_channel == NULL ||
        connection == NULL || event == NULL ||
        capacity == 0 || capacity > UINT32_MAX ||
        queue_depth <= 0 || queue_depth > INT32_MAX / 2) {
        errno = EINVAL;
        return -1;
    }
    if (*connection != NULL &&
        (*connection)->pending_cm_event != NULL) {
        errno = EBUSY;
        return -1;
    }

    kvs_rdma_connection_t *owner = calloc(1, sizeof(*owner));
    if (owner == NULL) {
        return -1;
    }

    int result = kvs_rdma_poll_cm_event(
        listener->event_channel, &owner->pending_cm_event);
    if (result <= 0) {
        free(owner);
        return result;
    }

    struct rdma_cm_event *cm_event = owner->pending_cm_event;
    int separate_owner = 1;
    int saved_errno = EPROTO;

    if (cm_event->event == RDMA_CM_EVENT_CONNECT_REQUEST &&
        cm_event->id != NULL &&
        cm_event->id != listener->listen_id) {
        owner->cm_id = cm_event->id;
        owner->cm_id->context = owner;

        if (*connection == NULL) {
            *connection = owner;
            separate_owner = 0;

            result = kvs_rdma_dispatch_cm_event(
                listener, cm_event, connection, event,
                capacity, queue_depth);
            saved_errno = errno;
            if (result < 0) {
                /* 尽力通知对端；原始错误和连接对象仍然保留。 */
                (void)rdma_reject(owner->cm_id, NULL, 0);
            }
        } else {
            /* 当前只接受一条连接，额外请求不影响现有连接。 */
            result = rdma_reject(owner->cm_id, NULL, 0);
            saved_errno = errno;
        }
    } else if (cm_event->event != RDMA_CM_EVENT_CONNECT_REQUEST &&
               *connection != NULL &&
               cm_event->id == (*connection)->cm_id) {
        (*connection)->pending_cm_event = owner->pending_cm_event;
        owner->pending_cm_event = NULL;
        free(owner);
        owner = *connection;
        separate_owner = 0;

        result = kvs_rdma_dispatch_cm_event(
            listener, cm_event, connection, event,
            capacity, queue_depth);
        saved_errno = errno;
    } else {
        result = -1;
    }

    /* 先确认事件，再允许关闭它关联的 CM ID。 */
    if (kvs_rdma_ack_cm_event(&owner->pending_cm_event) < 0) {
        event->connection = owner;
        return -1;
    }

    if (separate_owner) {
        if (kvs_rdma_connection_close(&owner) < 0) {
            event->connection = owner;
            return -1;
        }
        event->connection = NULL;
    } else if (result < 0) {
        event->connection = owner;
    }

    if (result < 0) {
        errno = saved_errno;
    }
    return result;
}

int kvs_rdma_connection_start(
    kvs_rdma_connection_t **connection,
    const char *host,
    unsigned short port,
    int timeout_ms)
{
    if (connection == NULL || host == NULL ||
        port == 0 || timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (*connection != NULL) {
        errno = EALREADY;
        return -1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
        address.sin_addr.s_addr == INADDR_ANY) {
        errno = EINVAL;
        return -1;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }

    kvs_rdma_connection_t *current = calloc(1, sizeof(*current));
    if (current == NULL) {
        return -1;
    }

    *connection = current;
    current->resolve_timeout_ms = timeout_ms;
    current->connect_deadline_ms =
        (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + timeout_ms;

    current->owned_cm_channel = rdma_create_event_channel();
    if (current->owned_cm_channel == NULL) {
        return -1;
    }

    int flags = fcntl(current->owned_cm_channel->fd, F_GETFL);
    if (flags < 0 ||
        fcntl(current->owned_cm_channel->fd,
              F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    if (rdma_create_id(
            current->owned_cm_channel,
            &current->cm_id,
            current,
            RDMA_PS_TCP) != 0) {
        return -1;
    }

    return rdma_resolve_addr(
        current->cm_id,
        NULL,
        (struct sockaddr *)&address,
        timeout_ms);
}

int kvs_rdma_connection_poll_event(
    kvs_rdma_connection_t *connection,
    kvs_rdma_event_t *event,
    size_t capacity,
    int queue_depth)
{
    if (event != NULL) {
        *event = (kvs_rdma_event_t){0};
    }

    if (connection == NULL || connection->cm_id == NULL ||
        connection->owned_cm_channel == NULL || event == NULL ||
        capacity == 0 || capacity > UINT32_MAX ||
        queue_depth <= 0 || queue_depth > INT32_MAX / 2) {
        errno = EINVAL;
        return -1;
    }

    if (connection->pending_cm_event != NULL) {
        event->connection = connection;
        errno = EBUSY;
        return -1;
    }

    /* rdma_connect 没有本项目的超时参数；无 CM 事件时也必须检查截止时间。 */
    if (connection->connect_deadline_ms != 0) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
            return -1;
        }
        int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (now_ms >= connection->connect_deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
    }

    int result = kvs_rdma_poll_cm_event(
        connection->owned_cm_channel,
        &connection->pending_cm_event);
    if (result <= 0) {
        return result;
    }

    result = kvs_rdma_handle_connection_event(
        connection,
        connection->pending_cm_event,
        event,
        capacity,
        queue_depth);
    int saved_errno = errno;

    if (kvs_rdma_ack_cm_event(
            &connection->pending_cm_event) < 0) {
        event->connection = connection;
        return -1;
    }

    if (result < 0) {
        event->connection = connection;
        errno = saved_errno;
    }

    return result;
}
