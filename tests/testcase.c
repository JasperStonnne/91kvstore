/*
 * 手动演示用的 C 客户端：一次命令只运行一个功能用例。
 * 启动 kvstore、发送 SAVE、宕机、重启，都由演示者自己操作。
 * 本程序不直接读写 snapshot.bin，也不会替 kvstore 恢复数据。
 *
 * 看懂“十万条写入与校验”，可以按这个顺序阅读：
 *   main             解析 write/verify、条数、起始编号等参数
 *   run_testcase     按编号循环，重复运行选中的一个用例
 *   testcase_write   生成 key/value，通过网络写入，检查 OK 回复
 *   testcase_verify  生成同编号的预期值，实际读取后逐字节比较
 *   make_record      写入和校验共同使用的数据生成规则
 *   text_equals      检查回复类型、长度和完整内容
 */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_RECORDS 10000000L

/* 服务端的一条 RESP 回复，不是从快照文件读取的数据。
 * type：'+' 表示状态（例如 OK），'-' 表示错误，':' 表示整数，'$' 表示字符串。
 * number：整数回复的值，或字符串的字节数；字符串长度 -1 表示 key 不存在。
 * data / length：实际收到的文本内容及其字节数，不包含 RESP 外层格式。
 */
typedef struct {
    char type;
    long number;
    size_t length;
    char data[1024];
} reply_t;

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s IP PORT ping|save\n"
        "  %s IP PORT OP COUNT [START=0] [DATASET=demo] [ENGINE=hash]\n"
        "OP: write verify modify verify-modified exists delete missing absent\n"
        "ENGINE: hash rbtree skiplist array (array range must be <=1024)\n"
        "Examples:\n"
        "  %s 127.0.0.1 29002 write 100000 0 snapshot\n"
        "  %s 127.0.0.1 29002 save\n"
        "  %s 127.0.0.1 29002 verify 100000 0 snapshot\n"
        "Only talks to the specified server; never starts/stops it.\n",
        program, program, program, program, program);
}

static int number(const char *text, long minimum, long maximum, long *result)
{
    if (*text < '0' || *text > '9') return -1;
    errno = 0;
    char *end;
    long value = strtol(text, &end, 10);
    if (errno || *end || value < minimum || value > maximum) return -1;
    *result = value;
    return 0;
}

/* TCP 的一次 send 不保证发完，所以循环发送，直到整条请求都发出去。 */
static int send_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t n = send(fd, data, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n;
        length -= (size_t)n;
    }
    return 0;
}

/* 从网络连接实际读取并解析一条回复；不是自己生成一个成功结果。 */
static int read_reply(FILE *reader, reply_t *reply)
{
    char line[256];
    if (!fgets(line, sizeof(line), reader)) return -1;
    size_t length = strlen(line);
    if (length < 3 || memcmp(line + length - 2, "\r\n", 2)) return -1;
    line[length - 2] = '\0';
    memset(reply, 0, sizeof(*reply));
    reply->type = line[0];
    if (line[0] == '+' || line[0] == '-') {
        reply->length = length - 3;
        memcpy(reply->data, line + 1, reply->length);
        return 0;
    }
    if (line[0] != ':' && line[0] != '$') return -1;
    errno = 0;
    char *end;
    reply->number = strtol(line + 1, &end, 10);
    if (errno || end == line + 1 || *end) return -1;
    if (line[0] == ':') return 0;
    if (reply->number == -1) return 0;
    if (reply->number < 0 || (size_t)reply->number >= sizeof(reply->data)) return -1;
    reply->length = (size_t)reply->number;
    if (fread(reply->data, 1, reply->length, reader) != reply->length) return -1;
    char trailer[2];
    if (fread(trailer, 1, 2, reader) != 2 || memcmp(trailer, "\r\n", 2)) return -1;
    return 0;
}

/* 将命令名、key、可选 value 编成 RESP 长度前缀请求。
 * 先发送给 kvstore，再读取 kvstore 的实际回复，存到 reply。
 */
static int command(int fd, FILE *reader, const char **fields, int count, reply_t *reply)
{
    char request[2048];
    size_t used = (size_t)snprintf(request, sizeof(request), "*%d\r\n", count);
    for (int i = 0; i < count; ++i) {
        int n = snprintf(request + used, sizeof(request) - used,
                         "$%zu\r\n%s\r\n", strlen(fields[i]), fields[i]);
        if (n < 0 || (size_t)n >= sizeof(request) - used) return -1;
        used += (size_t)n;
    }
    if (send_all(fd, request, used) < 0) return -1;
    return read_reply(reader, reply);
}

static double now(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}

/* 每一条数据的检查结果，交给公共循环分别计数。 */
typedef enum {
    CASE_PASS, CASE_MISSING, CASE_MISMATCH, CASE_ERROR
} case_result_t;

typedef struct {
    int fd;
    FILE *reader;
    const char *dataset; /* 例如 demo，只是拼进 key 的测试标记，不是数据库字段。 */
    const char *engine;  /* 引擎名；命令不指定时默认 hash。 */
    const char *prefix;  /* 命令前缀：Hash 为 H，RBTree 为 R，Skiplist 为 S，Array 为空。 */
} testcase_context_t;

/* 函数指针：传入连接信息和一个数据编号，检查这一条，返回检查结果。 */
typedef case_result_t (*testcase_fn)(testcase_context_t *, long);

typedef struct {
    const char *name;
    testcase_fn run;
    int single;
} testcase_entry_t;

/*
 * 同一个编号，每次生成相同的 key 和预期 value，因此重启客户端后仍能校验。
 * 例如 dataset=demo、engine=hash、index=78888 时：
 *   key = "manual demo hash 00078888"
 * 其中的空格也属于 key；demo、hash 只是 key 字符串中的文字。
 * %08ld 表示十进制编号至少占 8 位，不足时前面补 0。
 *
 * value 带有同一个编号；modified=0 生成原始值，modified=1 生成修改后的值。
 * value 中还包含空格、CRLF 换行、反斜杠和引号，用于检查特殊字符是否保持一致。
 * 本函数只在客户端内存里生成字符串，不发送命令，也不操作快照格式。
 */
static void make_record(testcase_context_t *test, long index, int modified,
                        char key[128], char value[256])
{
    snprintf(key, 128, "manual %s %s %08ld", test->dataset, test->engine, index);
    snprintf(value, 256, "value %s %08ld %s\r\nsecond line \\\"quoted\\\"",
             test->dataset, index, modified ? "modified" : "original");
}

/* 按引擎拼接命令名。例如 Hash 的前缀为 H，SET/GET 实际变成 HSET/HGET。
 * 再调用 command 完成真实的网络发送与接收。
 */
static int request(testcase_context_t *test, const char *verb, const char *key,
                   const char *value, reply_t *reply)
{
    char name[24];
    snprintf(name, sizeof(name), "%s%s", key ? test->prefix : "", verb);
    const char *fields[] = {name, key, value};
    if (command(test->fd, test->reader, fields, !key ? 1 : value ? 3 : 2, reply) < 0) {
        fprintf(stderr, "connection/protocol error (errno=%d)\n", errno);
        return -1;
    }
    if (reply->type == '-') {
        fprintf(stderr, "server error: %s\n", reply->data);
        return -1;
    }
    return 0;
}

/* 校验的关键：回复类型、长度、每个字节都必须符合预期。
 * memcmp 返回 0 才表示内容完全相同，前面的 ! 把这个结果转成“相同为真”。
 * 例如只读到了 key，却读回错误的 value，这里仍会返回假，不能计为通过。
 */
static int text_equals(const reply_t *reply, char type, const char *expected)
{
    return reply->type == type && reply->length == strlen(expected) &&
           !memcmp(reply->data, expected, reply->length);
}

/* 写入用例：负责一个编号的数据，外层 run_testcase 负责重复十万次。
 * 它只写入并检查服务端的 OK；持久化恢复是否正确，还要靠重启后的 verify 检查。
 */
static case_result_t testcase_write(testcase_context_t *test, long index)
{
    char key[128], value[256];
    reply_t reply;
    /* 1. 生成这个编号对应的 key 和原始 value。 */
    make_record(test, index, 0, key, value);
    /* 2. 发给 kvstore，收到实际回复；Hash 模式实际发送 HSET。 */
    if (request(test, "SET", key, value, &reply) < 0) return CASE_ERROR;
    /* 3. 只有服务端回复 +OK，这一条才算写入通过。 */
    return text_equals(&reply, '+', "OK") ? CASE_PASS : CASE_MISMATCH;
}

/* 读取校验用例：只 GET，不 SET，不会重新插入或修复任何数据。
 * expected 是客户端按原始规则生成的预期值；reply 是从 kvstore 实际读回的值。
 */
static case_result_t testcase_verify(testcase_context_t *test, long index)
{
    char key[128], expected[256];
    reply_t reply;
    /* 1. 用同一编号生成要读取的 key，以及当初应该写入的完整 value。 */
    make_record(test, index, 0, key, expected);
    /* 2. 实际向服务器读取这个 key；Hash 模式实际发送 HGET。 */
    if (request(test, "GET", key, NULL, &reply) < 0) return CASE_ERROR;
    /* 3. key 不存在，明确记为缺失，不能当成校验通过。 */
    if (reply.type == '$' && reply.number == -1) return CASE_MISSING;
    /* 4. 比较实际回复与预期值：类型、长度、每个字节都一致才通过。 */
    return text_equals(&reply, '$', expected) ? CASE_PASS : CASE_MISMATCH;
}

/* 修改用例：只修改已有 key，检查每条 MOD 的 OK 回复，不自动运行读取校验。 */
static case_result_t testcase_modify(testcase_context_t *test, long index)
{
    char key[128], value[256];
    reply_t reply;
    make_record(test, index, 1, key, value);
    if (request(test, "MOD", key, value, &reply) < 0) return CASE_ERROR;
    return text_equals(&reply, '+', "OK") ? CASE_PASS : CASE_MISMATCH;
}

/* 修改后校验用例：预期值改为 modified 版本，仍然只读取，不负责修改。 */
static case_result_t testcase_verify_modified(testcase_context_t *test, long index)
{
    char key[128], expected[256];
    reply_t reply;
    make_record(test, index, 1, key, expected);
    if (request(test, "GET", key, NULL, &reply) < 0) return CASE_ERROR;
    if (reply.type == '$' && reply.number == -1) return CASE_MISSING;
    return text_equals(&reply, '$', expected) ? CASE_PASS : CASE_MISMATCH;
}

/* 存在性用例：每个选中的 key 都必须存在，整数回复必须为 1。 */
static case_result_t testcase_exists(testcase_context_t *test, long index)
{
    char key[128], unused[256];
    reply_t reply;
    make_record(test, index, 0, key, unused);
    if (request(test, "EXIST", key, NULL, &reply) < 0) return CASE_ERROR;
    return reply.type == ':' && reply.number == 1 ? CASE_PASS : CASE_MISMATCH;
}

/* 删除用例：每条 DEL 必须实际删除一个 key，整数回复必须为 1。 */
static case_result_t testcase_delete(testcase_context_t *test, long index)
{
    char key[128], unused[256];
    reply_t reply;
    make_record(test, index, 0, key, unused);
    if (request(test, "DEL", key, NULL, &reply) < 0) return CASE_ERROR;
    return reply.type == ':' && reply.number == 1 ? CASE_PASS : CASE_MISMATCH;
}

/* 删除后读取用例：只读取，每个 key 都应返回 nil（不存在）。 */
static case_result_t testcase_missing(testcase_context_t *test, long index)
{
    char key[128], unused[256];
    reply_t reply;
    make_record(test, index, 0, key, unused);
    if (request(test, "GET", key, NULL, &reply) < 0) return CASE_ERROR;
    return reply.type == '$' && reply.number == -1 ? CASE_PASS : CASE_MISMATCH;
}

/* 不存在用例：检查每个 key 都不存在，整数回复必须为 0。 */
static case_result_t testcase_absent(testcase_context_t *test, long index)
{
    char key[128], unused[256];
    reply_t reply;
    make_record(test, index, 0, key, unused);
    if (request(test, "EXIST", key, NULL, &reply) < 0) return CASE_ERROR;
    return reply.type == ':' && reply.number == 0 ? CASE_PASS : CASE_MISMATCH;
}

static case_result_t testcase_ping(testcase_context_t *test, long index)
{
    (void)index;
    reply_t reply;
    if (request(test, "PING", NULL, NULL, &reply) < 0) return CASE_ERROR;
    return text_equals(&reply, '+', "PONG") ? CASE_PASS : CASE_MISMATCH;
}

/* 只有明确选择 save 才发送 SAVE，任何读写用例都不会自动调用这里。
 * 你现场演示时，直接使用 redis-cli -p 29002 SAVE 即可。
 */
static case_result_t testcase_save(testcase_context_t *test, long index)
{
    (void)index;
    reply_t reply;
    if (request(test, "SAVE", NULL, NULL, &reply) < 0) return CASE_ERROR;
    return text_equals(&reply, '+', "OK") ? CASE_PASS : CASE_MISMATCH;
}

/* 命令与独立测试函数的对应表：write 选择写入函数，verify 选择读取校验函数。
 * 最后一列为 1 表示只执行一次（PING/SAVE），0 表示按指定条数重复。
 */
static const testcase_entry_t testcases[] = {
    {"write", testcase_write, 0},
    {"verify", testcase_verify, 0},
    {"modify", testcase_modify, 0},
    {"verify-modified", testcase_verify_modified, 0},
    {"exists", testcase_exists, 0},
    {"delete", testcase_delete, 0},
    {"missing", testcase_missing, 0},
    {"absent", testcase_absent, 0},
    {"ping", testcase_ping, 1},
    {"save", testcase_save, 1},
};

/*
 * 十万条的循环就在这里，始终只重复用户选中的一个用例。
 * 命令传入 count=100000、start=0 时，i 依次为 0、1、…、99999。
 * 每个 i 都进入 make_record，因此操作十万个不同 key，不是反复操作同一个 key。
 */
static int run_testcase(testcase_context_t *test, const testcase_entry_t *entry,
                        long start, long count)
{
    double began = now();
    /* 成功数、缺失数、回复或内容不一致数、连接/协议/服务端错误数。 */
    long passed = 0, missing = 0, mismatched = 0, errors = 0;
    for (long i = start; i < start + count; ++i) {
        /* write 时调用 testcase_write；verify 时调用 testcase_verify。 */
        case_result_t result = entry->run(test, i);
        if (result == CASE_ERROR) {
            fprintf(stderr, "FAIL testcase=%s index=%ld\n", entry->name, i);
            errors++;
            break;
        }
        /* 只有这一条的检查确实通过，成功数才加 1。 */
        if (result == CASE_PASS) passed++;
        else {
            if (result == CASE_MISSING) missing++;
            else mismatched++;
            if (missing + mismatched <= 3)
                fprintf(stderr, "FAIL testcase=%s index=%ld: %s\n", entry->name, i,
                        result == CASE_MISSING ? "key missing" : "unexpected reply/value");
        }
        /* 每万条输出一次进度，不为每个 key 打印一行，避免十万条刷屏。 */
        if (!entry->single && (i - start + 1) % 10000 == 0)
            printf("progress=%ld/%ld passed=%ld\n", i - start + 1, count, passed);
    }
    /* 必须指定的全部条数都通过且没有错误，最后才能打印 PASS、返回 0。 */
    int success = passed == count && !errors;
    printf("%s op=%s expected=%ld passed=%ld missing=%ld mismatched=%ld errors=%ld seconds=%.3f\n",
           success ? "PASS" : "FAIL", entry->name, count, passed, missing, mismatched, errors, now() - began);
    return success ? 0 : 1;
}

int main(int argc, char **argv)
{
    /* 例：./bin/testcase 127.0.0.1 29002 verify 100000 0 demo
     * argv[1]=IP，argv[2]=端口，argv[3]=操作，argv[4]=条数，argv[5]=起始编号，
     * argv[6]=拼进 key 的标记，argv[7]=可选引擎（未填时为 hash）。
     */
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(argv[0]); return 0; }
    if (argc < 4) { usage(argv[0]); return 2; }
    long port, count = 1, start = 0;
    const char *operation = argv[3];
    const testcase_entry_t *selected = NULL;
    /* 从上面的对应表选中一个测试函数；这里不会自动执行整套测试。 */
    for (size_t i = 0; i < sizeof(testcases) / sizeof(testcases[0]); ++i)
        if (!strcmp(operation, testcases[i].name)) selected = &testcases[i];
    if (!selected) { usage(argv[0]); return 2; }
    int single = selected->single;
    if (number(argv[2], 1, 65535, &port) < 0 ||
        (single ? argc != 4 : (argc < 5 || argc > 8))) {
        usage(argv[0]); return 2;
    }
    if (!single && (number(argv[4], 1, MAX_RECORDS, &count) < 0 ||
        (argc > 5 && number(argv[5], 0, MAX_RECORDS, &start) < 0) ||
        start > MAX_RECORDS - count)) {
        fprintf(stderr, "Invalid COUNT/START (range limit: %ld).\n", MAX_RECORDS);
        return 2;
    }
    /* demo 只影响测试 key/value 的文字内容，不是给 kvstore 新增字段。 */
    const char *dataset = argc > 6 ? argv[6] : "demo";
    if (!*dataset || strlen(dataset) > 64 ||
        strspn(dataset, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(dataset)) {
        fprintf(stderr, "DATASET: 1..64 ASCII letters, digits, '_' or '-'.\n"); return 2;
    }
    /* 你之前的命令没填引擎，所以这十万条默认全部使用 Hash。 */
    const char *engine = argc > 7 ? argv[7] : "hash";
    const char *prefix = NULL;
    if (!strcmp(engine, "hash")) prefix = "H";
    else if (!strcmp(engine, "rbtree")) prefix = "R";
    else if (!strcmp(engine, "skiplist")) prefix = "S";
    else if (!strcmp(engine, "array")) prefix = "";
    if (!prefix || (!strcmp(engine, "array") && start + count > 1024)) {
        fprintf(stderr, "Invalid engine or array range exceeds its 1024-slot capacity.\n"); return 2;
    }
    /* 建立到指定 IP、端口的真实 TCP 连接；需要你先在另一个终端启动 kvstore。 */
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
        fprintf(stderr, "IP must be an IPv4 address.\n"); return 2;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct timeval timeout = {.tv_sec = 120};
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0 ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("connect/setup"); close(fd); return 1;
    }
    FILE *reader = fdopen(fd, "rb");
    if (!reader) { perror("fdopen"); close(fd); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("BEGIN op=%s target=%s:%ld engine=%s dataset=%s start=%ld count=%ld\n",
           operation, argv[1], port, engine, dataset, start, count);
    testcase_context_t test = {fd, reader, dataset, engine, prefix};
    /* 将选定的用例、起始编号、条数交给循环执行。 */
    int result = run_testcase(&test, selected, start, count);
    fclose(reader);
    return result;
}
