#ifndef KVS_REPLICATION_CAPTURE_IPC_H
#define KVS_REPLICATION_CAPTURE_IPC_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

/* 同机 SOCK_SEQPACKET 接口。一个请求只对应一个响应。 */
#define KVS_CAPTURE_READ_BYTES (64u * 1024u)
#define KVS_CAPTURE_READ_ERROR (-1)
#define KVS_CAPTURE_READ 0u
#define KVS_CAPTURE_BEGIN 1u

struct kvs_capture_read_request {
    uint64_t offset;
    uint32_t limit;
    uint32_t operation;
};

struct kvs_capture_read_response {
    int32_t length; /* 正数为字节数，0 为暂缺，-1 为缓存不可用。 */
    char data[KVS_CAPTURE_READ_BYTES];
};

static inline int kvs_capture_socket_address(struct sockaddr_un *address,
                                              const char *path)
{
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(address->sun_path)) {
        errno = EINVAL;
        return -1;
    }
    *address = (struct sockaddr_un){ .sun_family = AF_UNIX };
    memcpy(address->sun_path, path, length + 1);
    return 0;
}

static inline ssize_t kvs_capture_packet(int fd, void *data, size_t size, int sending)
{
    struct iovec iov = { .iov_base = data, .iov_len = size };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t result;
    do {
        result = sending ? sendmsg(fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL)
                         : recvmsg(fd, &message, MSG_DONTWAIT | MSG_TRUNC);
    } while (result < 0 && errno == EINTR);
    return result;
}

#endif
