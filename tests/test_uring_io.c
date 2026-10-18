#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include<string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <signal.h>
#include <sys/resource.h>
#include "uring_io.h"


static void test_short_write_then_failure(void)
{
    char path[] = "/tmp/91kv-uring-short-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    /* 保存原来的限制和信号处理方式。 */
    struct rlimit original_limit;
    assert(getrlimit(RLIMIT_FSIZE, &original_limit) == 0);

    struct sigaction original_action;
    struct sigaction ignored_action = {0};
    ignored_action.sa_handler = SIG_IGN;
    assert(sigemptyset(&ignored_action.sa_mask) == 0);
    assert(sigaction(
        SIGXFSZ, &ignored_action, &original_action) == 0);

    /* 只允许文件增长到 4 字节。 */
    struct rlimit limited = original_limit;
    limited.rlim_cur = 4;
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);

    kvs_uring_io_t io = {0};
    assert(kvs_uring_io_init(&io) == 0);

    const char data[] = "ABCDEFGHIJ";
    kvs_uring_write_t request = {
        .data = data,
        .length = sizeof(data) - 1,
        .offset = 0
    };

    int result = kvs_uring_io_write_batch(
        &io, fd, &request, 1);
    int write_errno = errno;

    /* 先恢复环境，再检查结果。 */
    assert(setrlimit(RLIMIT_FSIZE, &original_limit) == 0);
    assert(sigaction(SIGXFSZ, &original_action, NULL) == 0);

    assert(result == -1);
    assert(write_errno == EFBIG);

    /* 确认只写入了前 4 字节，没有误报完整成功。 */
    char actual[sizeof(data)] = {0};
    assert(pread(fd, actual, sizeof(actual), 0) == 4);
    assert(memcmp(actual, "ABCD", 4) == 0);

    /* 限制解除后，同一队列仍可正常写入完整数据。 */
    assert(kvs_uring_io_write_batch(
        &io, fd, &request, 1) == 0);
    assert(kvs_uring_io_sync(&io, fd) == 0);

    assert(pread(fd, actual, sizeof(actual), 0)
           == (ssize_t)(sizeof(data) - 1));
    assert(memcmp(actual, data, sizeof(data) - 1) == 0);

    kvs_uring_io_close(&io);
    assert(close(fd) == 0);
    assert(unlink(path) == 0);

    puts("[PASS] io_uring short write followed by failure");
}

int main(void)
{
    kvs_uring_io_t io = {0};

    kvs_uring_io_close(&io);

    assert(kvs_uring_io_init(&io) == 0);
    assert(io.initialized == 1);

    assert(kvs_uring_io_init(&io) == -1);
    assert(errno == EALREADY);

    kvs_uring_io_close(&io);
    assert(io.initialized == 0);
    kvs_uring_io_close(&io);

    assert(kvs_uring_io_init(&io) == 0);
    puts("[PASS] io_uring initialization and cleanup");

    /* 先用普通 write 准备数据，单独验证 io_uring 同步。 */
    char path[] = "/tmp/91kv-uring-io-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    const char data[] = "sync test\n";
    assert(write(fd, data, sizeof(data) - 1)
           == (ssize_t)(sizeof(data) - 1));

    assert(kvs_uring_io_sync(&io, fd) == 0);
    puts("[PASS] io_uring file sync");

    /* 清空测试文件，准备验证批量写入。 */
    assert(ftruncate(fd, 0) == 0);

    /*
     * 故意按不同于文件排列的顺序提供数据块。
     * 最终排列应由 offset 决定。
     */
    kvs_uring_write_t writes[] = {
        {.data = "FG",     .length = 2, .offset = 8},
        {.data = "ABCD",   .length = 4, .offset = 0},
        {.data = " E\r\n", .length = 4, .offset = 4}
    };

    assert(kvs_uring_io_write_batch(&io, fd, writes, 3) == 0);
    assert(kvs_uring_io_sync(&io, fd) == 0);

    const char expected[] = "ABCD E\r\nFG";
    char actual[sizeof(expected)];

    ssize_t read_bytes = pread(fd, actual, sizeof(actual), 0);
    assert(read_bytes == (ssize_t)(sizeof(expected) - 1));
    assert(memcmp(actual, expected, sizeof(expected) - 1) == 0);

    puts("[PASS] io_uring batch write and file positions");

    /* 两块数据写同一个位置，应在提交前被拒绝。 */
    kvs_uring_write_t overlapping[] = {
        {.data = "X", .length = 1, .offset = 0},
        {.data = "Y", .length = 1, .offset = 0}
    };

    assert(kvs_uring_io_write_batch(
        &io, fd, overlapping, 2) == -1);
    assert(errno == EINVAL);

    /* 本模块按指定位置写入，不接受 O_APPEND 文件。 */
    int append_fd = open(path, O_WRONLY | O_APPEND);
    assert(append_fd >= 0);

    assert(kvs_uring_io_write_batch(
        &io, append_fd, writes, 3) == -1);
    assert(errno == EINVAL);
    assert(close(append_fd) == 0);

    puts("[PASS] io_uring rejects overlapping and append writes");

    /*
     * 用只读文件描述符提交 WRITE。
     * 这次请求会交给内核，再从完成结果中得到 EBADF。
     */
    int readonly_fd = open(path, O_RDONLY);
    assert(readonly_fd >= 0);

    assert(kvs_uring_io_write_batch(
        &io, readonly_fd, writes, 3) == -1);
    assert(errno == EBADF);
    assert(io.initialized == 1);
    assert(close(readonly_fd) == 0);

    /* 上面的失败操作不能改变原来的文件内容。 */
    read_bytes = pread(fd, actual, sizeof(actual), 0);
    assert(read_bytes == (ssize_t)(sizeof(expected) - 1));
    assert(memcmp(actual, expected, sizeof(expected) - 1) == 0);

    /*
     * 再次正常写入并同步。
     * 验证失败批次的结果已收齐，没有干扰后续操作。
     */
    assert(kvs_uring_io_write_batch(&io, fd, writes, 3) == 0);
    assert(kvs_uring_io_sync(&io, fd) == 0);

    puts("[PASS] io_uring write failure and queue reuse");
    /* 模拟写入目标空间不足，检查完成结果中的错误。 */
    int full_fd = open("/dev/full", O_WRONLY | O_CLOEXEC);
    assert(full_fd >= 0);

    kvs_uring_write_t full_request = {
        .data = "test",
        .length = 4,
        .offset = 0
    };

    assert(kvs_uring_io_write_batch(
        &io, full_fd, &full_request, 1) == -1);
    assert(errno == ENOSPC);
    assert(close(full_fd) == 0);

    /* 失败批次处理结束后，正常文件仍应能写入和同步。 */
    assert(kvs_uring_io_write_batch(
        &io, fd, writes, 3) == 0);
    assert(kvs_uring_io_sync(&io, fd) == 0);

    puts("[PASS] io_uring ENOSPC propagation and queue reuse");

    /*
     * 关闭文件后再同步，内核应报告 EBADF。
     * 此处不打开其他文件，避免这个 fd 被重新使用。
     */
    assert(close(fd) == 0);
    assert(kvs_uring_io_sync(&io, fd) == -1);
    assert(errno == EBADF);

    /* 一次同步操作失败，队列仍能正常清理。 */
    assert(io.initialized == 1);
    assert(unlink(path) == 0);
    kvs_uring_io_close(&io);

    puts("[PASS] io_uring sync error propagation");
        test_short_write_then_failure();
    return 0;
}