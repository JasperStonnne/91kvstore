#define _DEFAULT_SOURCE
#include "rdma_transport.h"
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_BYTES (1024u * 1024u)
static double deadline;

static void fail(const char *operation)
{
    perror(operation);
    exit(1);
}

static double now(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0) {
        fail("clock_gettime");
    }
    return value.tv_sec + value.tv_nsec / 1e9;
}

static void check_deadline(void)
{
    if (now() > deadline) {
        errno = ETIMEDOUT;
        fail("transfer timeout");
    }
}

/* CRC 在传输计时区间外计算；两种路径使用相同的字节校验。 */
static uint32_t crc32(const unsigned char *data, size_t length)
{
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t value = i;
        for (unsigned int bit = 0; bit < 8; bit++) {
            value = (value >> 1) ^ ((value & 1u) ? UINT32_C(0xedb88320) : 0);
        }
        table[i] = value;
    }
    uint32_t value = UINT32_C(0xffffffff);
    for (size_t i = 0; i < length; i++) {
        value = table[(value ^ data[i]) & 255u] ^ (value >> 8);
    }
    return value ^ UINT32_C(0xffffffff);
}

static void packet(int fd, void *data, size_t length, int sending)
{
    unsigned char *bytes = data;
    while (length > 0) {
        ssize_t count = sending ? send(fd, bytes, length, MSG_NOSIGNAL)
                                : recv(fd, bytes, length, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (count == 0) {
                errno = ECONNRESET;
            }
            fail("TCP control/data");
        }
        bytes += count;
        length -= (size_t)count;
    }
}

static int tcp_socket(const char *host, unsigned short port, int receiving)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("socket");
    }
    int enabled = 1;
    struct timeval timeout = {.tv_sec = 60};
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        fail("setsockopt");
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        errno = EINVAL;
        fail("IPv4 address");
    }
    if (!receiving) {
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            fail("connect");
        }
        return fd;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 1) < 0) {
        fail("listen");
    }
    printf("READY\n");
    fflush(stdout);
    int accepted = accept(fd, NULL, NULL);
    close(fd);
    if (accepted < 0) {
        fail("accept");
    }
    if (setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0 ||
        setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(accepted, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        fail("accepted timeout");
    }
    return accepted;
}

static void wait_event(kvs_rdma_listener_t *listener, kvs_rdma_connection_t **connection,
                        kvs_rdma_event_type_t expected)
{
    while (1) {
        check_deadline();
        kvs_rdma_event_t event;
        int result = listener ? kvs_rdma_listener_poll(listener, connection, &event, CHUNK_BYTES, 1)
                              : kvs_rdma_connection_poll_event(*connection, &event, CHUNK_BYTES, 1);
        if (result < 0) {
            fail("RDMA CM");
        }
        if (result > 0 && event.type == expected) {
            return;
        }
        usleep(100);
    }
}

static kvs_rdma_completion_t wait_completion(kvs_rdma_connection_t *connection,
                                             kvs_rdma_completion_type_t expected)
{
    while (1) {
        check_deadline();
        kvs_rdma_completion_t completion;
        int result = kvs_rdma_connection_poll_completion(connection, &completion);
        if (result < 0) {
            fail("RDMA CQ");
        }
        if (result > 0) {
            if (completion.type != expected) {
                errno = EPROTO;
                fail("unexpected CQ type");
            }
            return completion;
        }
    }
}

int main(int argc, char **argv)
{
    if ((argc != 5 && argc != 6) ||
        (strcmp(argv[1], "send") && strcmp(argv[1], "receive")) ||
        (strcmp(argv[2], "rdma") && strcmp(argv[2], "sendfile"))) {
        fprintf(stderr, "usage: %s send|receive rdma|sendfile HOST PORT [FILE]\n", argv[0]);
        return 2;
    }
    int receiving = strcmp(argv[1], "receive") == 0;
    int rdma = strcmp(argv[2], "rdma") == 0;
    char *end;
    errno = 0;
    long port = strtol(argv[4], &end, 10);
    if (errno || *end || port < 1 || port > 65534 || argc != (receiving ? 5 : 6)) {
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    alarm(60);
    deadline = now() + 60;
    int file_fd = -1;
    unsigned char *data = NULL;
    size_t length = 0;
    uint32_t checksum = 0;
    double checksum_seconds = 0;
    if (!receiving) {
        file_fd = open(argv[5], O_RDONLY | O_CLOEXEC);
        struct stat info;
        if (file_fd < 0 || fstat(file_fd, &info) < 0) {
            fail("input file");
        }
        if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uintmax_t)info.st_size > SIZE_MAX) {
            errno = EINVAL;
            fail("file size");
        }
        length = (size_t)info.st_size;
        if (length > 0) {
            data = mmap(NULL, length, PROT_READ, MAP_PRIVATE, file_fd, 0);
            if (data == MAP_FAILED) {
                fail("mmap input");
            }
        }
        double crc_started = now();
        checksum = crc32(data, length); /* 同时预热文件页，两条路径一致。 */
        checksum_seconds = now() - crc_started;
    }
    kvs_rdma_listener_t *listener = NULL;
    kvs_rdma_connection_t *connection = NULL;
    if (receiving && rdma) {
        listener = kvs_rdma_listener_create();
        if (!listener || kvs_rdma_listener_start(listener, argv[3], (unsigned short)(port + 1), 1) < 0) {
            fail("RDMA listener");
        }
    }
    double setup_started = now();
    int tcp = tcp_socket(argv[3], (unsigned short)port, receiving);
    /* 长度、CRC 和路径类型均使用明确的网络字节序。 */
    uint64_t wire_length = htobe64(length);
    uint32_t wire_fields[2] = {htonl(checksum), htonl((uint32_t)rdma)};
    if (receiving) {
        packet(tcp, &wire_length, sizeof(wire_length), 0);
        packet(tcp, wire_fields, sizeof(wire_fields), 0);
        if (be64toh(wire_length) > SIZE_MAX || ntohl(wire_fields[1]) != (uint32_t)rdma) {
            errno = EPROTO;
            fail("transfer header");
        }
        length = (size_t)be64toh(wire_length);
        checksum = ntohl(wire_fields[0]);
        data = malloc(length ? length : 1);
        if (!data) {
            fail("receive buffer");
        }
    } else {
        packet(tcp, &wire_length, sizeof(wire_length), 1);
        packet(tcp, wire_fields, sizeof(wire_fields), 1);
    }
    if (rdma) {
        if (!receiving && kvs_rdma_connection_start(&connection, argv[3],
                                                    (unsigned short)(port + 1), 5000) < 0) {
            fail("RDMA connect");
        }
        wait_event(listener, &connection, KVS_RDMA_EVENT_ESTABLISHED);
    }
    char marker = 'R';
    packet(tcp, &marker, 1, receiving);
    if (marker != 'R') {
        errno = EPROTO;
        fail("ready marker");
    }
    double setup_seconds = now() - setup_started;
    double started = now();
    size_t offset = 0;
    while (offset < length) {
        size_t bytes = length - offset;
        if (bytes > CHUNK_BYTES) {
            bytes = CHUNK_BYTES;
        }
        if (rdma && receiving) {
            kvs_rdma_completion_t completion = wait_completion(connection, KVS_RDMA_COMPLETION_RECV);
            if (completion.length != bytes) {
                errno = EPROTO;
                fail("RDMA chunk length");
            }
            memcpy(data + offset, completion.data, bytes);
            if (kvs_rdma_connection_receive_next(connection) < 0) {
                fail("RDMA receive next");
            }
        } else if (rdma) {
            if (kvs_rdma_connection_send(connection, data + offset, bytes) < 0) {
                fail("RDMA send");
            }
            wait_completion(connection, KVS_RDMA_COMPLETION_SEND);
        } else if (receiving) {
            packet(tcp, data + offset, bytes, 0);
        } else {
            off_t file_offset = (off_t)offset;
            size_t remaining = bytes;
            while (remaining > 0) {
                ssize_t sent = sendfile(tcp, file_fd, &file_offset, remaining);
                if (sent < 0 && errno == EINTR) {
                    continue;
                }
                if (sent <= 0) {
                    fail("sendfile");
                }
                remaining -= (size_t)sent;
            }
        }
        offset += bytes;
        marker = 'A';
        packet(tcp, &marker, 1, receiving); /* 两条路径均为 1 MiB、一个在途块。 */
        if (marker != 'A') {
            errno = EPROTO;
            fail("chunk acknowledgement");
        }
    }
    double transfer_seconds = now() - started;
    int verified = 1;
    if (receiving) {
        started = now();
        uint32_t actual = crc32(data, length);
        checksum_seconds = now() - started;
        verified = actual == checksum;
        checksum = actual;
    }
    marker = verified ? 'V' : 'F';
    packet(tcp, &marker, 1, receiving); /* 最终校验发生在传输计时之后。 */
    verified = marker == 'V';
    if (rdma) {
        if (!receiving && kvs_rdma_connection_disconnect(connection) < 0) {
            fail("RDMA disconnect");
        }
        wait_event(listener, &connection, KVS_RDMA_EVENT_DISCONNECTED);
        if (kvs_rdma_connection_close(&connection) < 0 || kvs_rdma_listener_close(&listener) < 0) {
            fail("RDMA close");
        }
    }
    printf("{\"mode\":\"%s\",\"role\":\"%s\",\"bytes\":%zu,\"chunk_bytes\":%u,"
           "\"window\":1,\"setup_seconds\":%.9f,\"transfer_seconds\":%.9f,"
           "\"checksum_seconds\":%.9f,\"crc32\":\"%08" PRIx32 "\",\"verified\":%s}\n",
           argv[2], argv[1], length, CHUNK_BYTES, setup_seconds, transfer_seconds,
           checksum_seconds, checksum, verified ? "true" : "false");
    close(tcp);
    if (receiving) {
        free(data);
    } else {
        if (length > 0) {
            munmap(data, length);
        }
        close(file_fd);
    }
    return verified ? 0 : 1;
}
