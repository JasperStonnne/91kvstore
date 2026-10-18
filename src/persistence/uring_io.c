#include "uring_io.h"

#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

/* 一块数据在本次批量写入中的进度，仅供本模块使用。 */
typedef struct {
    size_t completed_bytes; /* 已经确认写成功的字节数。 */
    size_t requested_bytes; /* 当前这一次请求要求写多少字节。 */
} uring_write_progress_t;

/*
 * 为尚未写完的数据准备 WRITE 请求。
 * 这里只填写请求，不提交给内核。
 *
 * 参数、文件范围和进度有效性由调用方保证。
 * 返回准备的请求数量；失败返回 -1。
 * 失败时可能已准备部分请求，由调用方负责清理。
 */
static int uring_prepare_writes(
    kvs_uring_io_t *io,
    int fd,
    const kvs_uring_write_t *writes,
    uring_write_progress_t *progress,
    size_t count)
{
    int prepared_count = 0;

    for (size_t i = 0; i < count; ++i) {
        /* 取出这一块的数据描述和对应进度，不复制数据。 */
        const kvs_uring_write_t *block = &writes[i];
        uring_write_progress_t *state = &progress[i];

        state->requested_bytes = 0;

        /* 整块数据已经写完，就跳过。 */
        if (state->completed_bytes == block->length) {
            continue;
        }

        size_t completed_bytes = state->completed_bytes;
        size_t remaining_bytes = block->length - completed_bytes;

        /* 单次请求长度不超过完成结果能够表示的范围。 */
        size_t request_bytes = remaining_bytes;
        if (request_bytes > (size_t)INT_MAX) {
            request_bytes = (size_t)INT_MAX;
        }

        /* 从内存中已经写完的部分之后，继续取数据。 */
        const unsigned char *data_start =
            (const unsigned char *)block->data + completed_bytes;

        /* 文件中的写入位置，也前进相同的字节数。 */
        off_t file_position =
            block->offset + (off_t)completed_bytes;

        /* 取得一个空白请求位置。 */
        struct io_uring_sqe *sqe = io_uring_get_sqe(&io->ring);
        if (sqe == NULL) {
            errno = EBUSY;
            return -1;
        }

        /* 填写：从哪里取、取多少、写到文件哪里。 */
        io_uring_prep_write(
            sqe,
            fd,
            data_start,
            (unsigned int)request_bytes,
            file_position
        );

        /* 完成结果通过这个编号找到对应的进度。 */
        io_uring_sqe_set_data64(sqe, i);

        state->requested_bytes = request_bytes;
        prepared_count++;
    }

    return prepared_count;
}

/*
 * 提交本轮准备好的请求。
 *
 * prepared_count：本轮准备了多少个请求。
 * submitted_count：输出实际已经提交了多少个。
 *
 * 成功返回 0；失败返回 -1，并设置 errno。
 * 即使失败，submitted_count 仍保留已提交的数量，
 * 供后面的代码等待这些请求结束。
 */
static int uring_submit_prepared(
    kvs_uring_io_t *io,
    int prepared_count,
    int *submitted_count)
{
    *submitted_count = 0;

    while (*submitted_count < prepared_count) {
        /* 返回值表示本次实际提交了几个请求。 */
        int result = io_uring_submit(&io->ring);

        /* 提交被信号中断，重试。 */
        if (result == -EINTR) {
            continue;
        }

        if (result < 0) {
            errno = -result;
            return -1;
        }

        /*
         * 仍有请求需要提交，却没有提交任何一个。
         * 返回错误，避免循环一直没有进展。
         */
        if (result == 0) {
            errno = EIO;
            return -1;
        }

        /* 累计已经交给内核的请求数量。 */
        *submitted_count += result;
    }

    return 0;
}

/*
 * 等待并取回一个 WRITE 完成结果。
 *
 * block_index：输出数据块的编号。
 * write_result：输出写入结果：
 *     > 0：实际写成功的字节数。
 *     = 0：没有写入任何字节。
 *     < 0：负错误码。
 *
 * 本函数返回 0，只表示成功取得结果。
 * 返回 -1 表示等待失败，此时可能仍有请求正在执行，
 * 调用方不能直接释放缓冲区。
 */
static int uring_receive_write_result(
    kvs_uring_io_t *io,
    size_t *block_index,
    int *write_result)
{
    struct io_uring_cqe *cqe = NULL;
    int result;

    /* 等待一个完成结果；信号中断时重试等待。 */
    do {
        result = io_uring_wait_cqe(&io->ring, &cqe);
    } while (result == -EINTR);

    if (result < 0) {
        errno = -result;
        return -1;
    }

    /* 取出准备请求时填写的编号。 */
    *block_index = (size_t)cqe->user_data;

    /* 取出内核报告的实际写入结果。 */
    *write_result = cqe->res;

    /*
     * 两个结果已经复制到调用方提供的变量中，
     * 现在可以回收这个完成队列位置。
     */
    io_uring_cqe_seen(&io->ring, cqe);

    return 0;
}

/*
 * 异常时终止本轮请求并清理队列。
 *
 * pending_count：已经提交、但尚未取回完成结果的请求数量。
 *
 * 返回前，必须确认这些请求已经结束。
 * 无法确认时，按已确定的策略明确报错并终止进程。
 */
static void uring_abort_requests(
    kvs_uring_io_t *io,
    int pending_count)
{
    /* 保留最初导致本轮失败的错误。 */
    int original_error = errno;

    if (pending_count > 0) {
        /*
         * 批量请求的编号为 0 到队列容量减一。
         * 按编号逐个尝试同步取消。
         */
        for (unsigned int i = 0; i < KVS_URING_QUEUE_DEPTH; ++i) {
            struct io_uring_sync_cancel_reg cancel = {0};

            cancel.addr = i;    /* 匹配请求的 user_data 编号。 */
            cancel.flags = 0;   /* 按编号匹配单个请求。 */

            /* 不额外设置取消等待期限。 */
            cancel.timeout.tv_sec = -1;
            cancel.timeout.tv_nsec = -1;

            int result;
            do {
                result = io_uring_register_sync_cancel(
                    &io->ring, &cancel);
            } while (result == -EINTR);

            /*
             * ENOENT 表示没找到请求，例如它已经完成。
             * 其他取消错误意味着无法可靠完成这次收尾。
             */
            if (result < 0 && result != -ENOENT) {
                fprintf(
                    stderr,
                    "fatal: io_uring cancellation failed: %s\n",
                    strerror(-result)
                );
                fflush(stderr);
                _Exit(EXIT_FAILURE);
            }
        }

        /*
         * 取消之后，仍要取回每个尚未收到的完成结果。
         * 不能只看到“取消调用成功”就释放缓冲区。
         */
        for (int received = 0;
             received < pending_count;
             ++received) {
            size_t block_index;
            int write_result;

            if (uring_receive_write_result(
                    io, &block_index, &write_result) < 0) {
                fprintf(
                    stderr,
                    "fatal: cannot reap io_uring requests: %s\n",
                    strerror(errno)
                );
                fflush(stderr);
                _Exit(EXIT_FAILURE);
            }

            /*
             * 收尾时只需要确认请求已经结束。
             * 写入失败或被取消的结果也算已经结束。
             */
        }
    }

    /*
     * 已提交请求已收尾，再释放队列。
     * 同时丢弃队列中可能尚未提交的请求。
     * 调用方拥有的文件描述符仍由调用方关闭。
     */
    kvs_uring_io_close(io);

    errno = original_error;
}

int kvs_uring_io_init(kvs_uring_io_t *io)
{
    if (io == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 已经持有队列时，不能再次初始化，避免丢失原来的资源。 */
    if (io->initialized) {
        errno = EALREADY;
        return -1;
    }

    /*
     * 创建提交队列和完成队列。
     * 最后一个参数 0：暂时不启用额外的 setup 选项。
     */
    int result = io_uring_queue_init(
        KVS_URING_QUEUE_DEPTH,
        &io->ring,
        0
    );

    /*
     * liburing 失败时返回负错误码。
     * 转换成项目采用的：返回 -1，具体原因放入 errno。
     */
    if (result < 0) {
        errno = -result;
        return -1;
    }

    io->initialized = 1;
    return 0;
}

void kvs_uring_io_close(kvs_uring_io_t *io)
{
    /* 空对象、尚未初始化或已经关闭的对象，无须释放。 */
    if (io == NULL || !io->initialized) {
        return;
    }

    /* 清理资源时，保留调用方可能正在处理的错误。 */
    int saved_errno = errno;

    /* 释放队列映射，并关闭 io_uring 自身的文件描述符。 */
    io_uring_queue_exit(&io->ring);

    /* 恢复为空对象，允许以后重新初始化。 */
    memset(io, 0, sizeof(*io));

    errno = saved_errno;
}

int kvs_uring_io_sync(kvs_uring_io_t *io, int fd)
{
    if (io == NULL || !io->initialized || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        /* 1. 从提交队列取得一个请求位置。 */
        struct io_uring_sqe *sqe = io_uring_get_sqe(&io->ring);
        if (sqe == NULL) {
            errno = EBUSY;
            return -1;
        }

        /*
         * 2. 填写文件同步请求。
         * flags=0，使用完整 fsync，不改成 fdatasync。
         */
        io_uring_prep_fsync(sqe, fd, 0);
        io_uring_sqe_set_data64(sqe, 0);

        /* 3. 提交。信号中断时重试。 */
        int result;
        do {
            result = io_uring_submit(&io->ring);
        } while (result == -EINTR);

        /* 本次准备了一个请求，必须确认提交了一个。 */
        if (result != 1) {
            int saved_errno = result < 0 ? -result : EIO;

            /* 提交异常后清理队列，避免后续操作使用遗留请求。 */
            kvs_uring_io_close(io);
            errno = saved_errno;
            return -1;
        }

        /* 4. 等待这个请求的完成结果。 */
        struct io_uring_cqe *cqe = NULL;
        do {
            result = io_uring_wait_cqe(&io->ring, &cqe);
        } while (result == -EINTR);

        if (result < 0) {
            int saved_errno = -result;
            kvs_uring_io_close(io);
            errno = saved_errno;
            return -1;
        }

        /*
         * 5. 先取出同步结果，再回收完成队列中的这个位置。
         * 回收后，不再通过 cqe 指针读取结果。
         */
        int sync_result = cqe->res;
        io_uring_cqe_seen(&io->ring, cqe);

        /* 同步操作本身被信号中断时，重新发起同步。 */
        if (sync_result == -EINTR) {
            continue;
        }

        if (sync_result != 0) {
            errno = sync_result < 0 ? -sync_result : EIO;
            return -1;
        }

        return 0;
    }
}

int kvs_uring_io_write_batch(
    kvs_uring_io_t *io,
    int fd,
    const kvs_uring_write_t *writes,
    size_t count)
{
    if (io == NULL || !io->initialized || fd < 0 ||
        count > KVS_URING_QUEUE_DEPTH ||
        (count > 0 && writes == NULL)) {
        errno = EINVAL;
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    /*
     * 本接口按指定 offset 写入。
     * 拒绝 O_APPEND，避免文件追加模式干扰指定位置。
     */
    int file_flags = fcntl(fd, F_GETFL);
    if (file_flags < 0) {
        return -1;
    }
    if (file_flags & O_APPEND) {
        errno = EINVAL;
        return -1;
    }

    /*
     * 开始写入前，检查整批数据。
     * range_ends 保存每块数据在文件中的结束位置。
     */
    off_t range_ends[KVS_URING_QUEUE_DEPTH];

    for (size_t i = 0; i < count; ++i) {
        if (writes[i].offset < 0 ||
            (writes[i].length > 0 && writes[i].data == NULL)) {
            errno = EINVAL;
            return -1;
        }

        /* 检查 offset + length 是否超出 off_t 可表示范围。 */
        if (__builtin_add_overflow(
                writes[i].offset,
                writes[i].length,
                &range_ends[i])) {
            errno = EOVERFLOW;
            return -1;
        }

        /* 空块没有实际写入范围。 */
        if (writes[i].length == 0) {
            continue;
        }

        /* 各块写入范围不能重叠。 */
        for (size_t j = 0; j < i; ++j) {
            if (writes[j].length == 0) {
                continue;
            }

            if (writes[i].offset < range_ends[j] &&
                writes[j].offset < range_ends[i]) {
                errno = EINVAL;
                return -1;
            }
        }
    }

    /* 每块数据都从“尚未写入任何字节”开始。 */
    uring_write_progress_t progress[KVS_URING_QUEUE_DEPTH] = {0};

    for (;;) {
        int prepared_count = uring_prepare_writes(
            io, fd, writes, progress, count);

        if (prepared_count < 0) {
            /*
             * 本轮还没提交，清理可能已经准备的请求。
             * 前一轮的请求已经全部回收。
             */
            uring_abort_requests(io, 0);
            return -1;
        }

        /* 没有需要准备的请求，说明全部字节已经写完。 */
        if (prepared_count == 0) {
            return 0;
        }

        int submitted_count = 0;

        if (uring_submit_prepared(
                io, prepared_count, &submitted_count) < 0) {
            /* 提交失败，也要安全收尾已经提交的那部分。 */
            uring_abort_requests(io, submitted_count);
            return -1;
        }

        /*
         * 即使某个 WRITE 失败，也继续收齐其他完成结果。
         * first_error 保留本轮最先发现的错误。
         */
        int first_error = 0;

        for (int received_count = 0;
             received_count < submitted_count;
             ++received_count) {
            size_t block_index;
            int write_result;

            if (uring_receive_write_result(
                    io, &block_index, &write_result) < 0) {
                if (first_error != 0) {
                    errno = first_error;
                }

                /* 本次等待失败，没有取走一个完成结果。 */
                int pending_count =
                    submitted_count - received_count;

                uring_abort_requests(io, pending_count);
                return -1;
            }

            /* 检查结果编号，避免访问错误的进度记录。 */
            if (block_index >= count ||
                progress[block_index].requested_bytes == 0) {
                if (first_error == 0) {
                    first_error = EIO;
                }
                continue;
            }

            uring_write_progress_t *state =
                &progress[block_index];

            size_t requested_bytes = state->requested_bytes;

            /* 本轮这块数据的完成结果已经收到。 */
            state->requested_bytes = 0;

            /* 写操作被中断，留到下一轮重新请求剩余数据。 */
            if (write_result == -EINTR) {
                continue;
            }

            /* 零进展、写入失败或异常的返回长度，都算失败。 */
            if (write_result <= 0 ||
                (size_t)write_result > requested_bytes) {
                if (first_error == 0) {
                    first_error =
                        write_result < 0 ? -write_result : EIO;
                }
                continue;
            }

            /* 只累计内核确认写成功的字节数。 */
            state->completed_bytes += (size_t)write_result;
        }

        /* 所有已提交请求都已回收，现在可以安全返回失败。 */
        if (first_error != 0) {
            errno = first_error;
            return -1;
        }

        /* 本轮成功；下一轮继续处理短写留下的部分。 */
    }
}