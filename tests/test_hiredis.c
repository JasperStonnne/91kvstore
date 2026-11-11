/*
 * hiredis 兼容性测试：用现成的 C 客户端库操作你已经启动的 kvstore。
 * 运行方式：./bin/test_hiredis 29002
 *
 * 本程序只连接本机指定端口，不启动/停止服务，也不发送 SAVE。
 * 下面的 8 项检查顺序执行；任意一项失败就结束，全部通过才返回 0。
 * 每次 redisCommand 都实际发送命令并等待回复，不是直接打印一个 PASS。
 */
#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 检查一条实际回复：
 *   context          当前连接，用来获取连接错误信息。
 *   name             这项检查的名字，用于输出 PASS/FAIL。
 *   reply            redisCommand 从服务端收到并解析出的回复。
 *   expected_type    预期回复类型：状态、字符串、整数、空值或错误。
 *   expected_text    要比较的完整文本；为 NULL 时不检查文本。
 *   expected_integer 整数回复的预期值；其他类型不使用这个参数。
 *
 * 返回 0 表示符合预期，-1 表示检查失败；函数最后释放这条回复。
 */
static int check_reply(
    redisContext *context, const char *name, redisReply *reply,
    int expected_type, const char *expected_text, long long expected_integer)
{
    /* 必须真的收到回复，并且回复类型正确，才能继续检查内容。 */
    int ok = reply != NULL && reply->type == expected_type;

    if (ok && expected_text != NULL) {
        /* 长度和每个字节都要相同；memcmp 返回 0 表示完全一致。 */
        ok = reply->str != NULL &&
             reply->len == strlen(expected_text) &&
             memcmp(reply->str, expected_text, reply->len) == 0;
    }
    if (ok && expected_type == REDIS_REPLY_INTEGER) {
        /* 例如 DEL 需要返回整数 1，表示实际删除了一个 key。 */
        ok = reply->integer == expected_integer;
    }

    if (!ok) {
        fprintf(stderr, "[FAIL] %s: reply type=%d, connection error=%s\n",
                name, reply != NULL ? reply->type : -1, context->errstr);
    } else {
        printf("[PASS] %s\n", name);
    }

    /* 回复由 hiredis 分配，用完必须释放。 */
    if (reply != NULL) freeReplyObject(reply);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    /* 只接收一个端口参数，例如 argv[1] 是 "29002"。 */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 2;
    }

    /* 建立到你自己启动的 kvstore 的连接。
     * atoi 把端口字符串转换为整数，context 保存连接状态。
     */
    redisContext *context = redisConnect("127.0.0.1", atoi(argv[1]));
    if (context == NULL || context->err) {
        fprintf(stderr, "hiredis connection failed: %s\n",
                context != NULL ? context->errstr : "out of memory");
        if (context != NULL) redisFree(context);
        return 1;
    }

    char key[64];
    /* 用本测试进程的 PID 生成测试 key，例如 hiredis-probe-12345。
     * 后面的写入、读取、覆盖、删除都针对这个 key，不使用 demo 那批 key。
     */
    snprintf(key, sizeof(key), "hiredis-probe-%ld", (long)getpid());
    /* value 含空格和真实的 CRLF 换行，用来检查特殊字符是否原样传输。 */
    const char value[] = "a b\r\nc";
    /* 默认失败，只有下面 8 项全通过后才改成 0。 */
    int result = 1;

    /* 1. 发送 PING，要求收到状态回复 PONG，确认命令交互正常。 */
    if (check_reply(context, "PING", redisCommand(context, "PING"),
                    REDIS_REPLY_STATUS, "PONG", 0) < 0) goto done;
    /* 2. 发送 SET，写入 key 和含空格、换行的 value，要求状态回复 OK。
     * hiredis 的 %s 表示字符串参数；%b 接收“数据指针 + 字节长度”，
     * 因此空格和 CRLF 不会把一个 value 拆成多个命令参数。
     * sizeof(value)-1 排除 C 字符串末尾的 '\0'，不把它写进 value。
     */
    if (check_reply(context, "SET", redisCommand(
                        context, "SET %s %b", key, value, sizeof(value) - 1),
                    REDIS_REPLY_STATUS, "OK", 0) < 0) goto done;
    /* 3. 发送 GET，要求收到字符串，并逐字节等于刚才的 value。
     * 这一步才确认数据内容正确，不只是 SET 回复过 OK。
     */
    if (check_reply(context, "GET with spaces and CRLF",
                    redisCommand(context, "GET %s", key),
                    REDIS_REPLY_STRING, value, 0) < 0) goto done;
    /* 4. 对同一个 key 再次 SET，把 value 覆盖成 other，要求回复 OK。
     * 这是当前项目已有的 SET 覆盖语义，不是新增服务器行为。
     */
    if (check_reply(context, "duplicate SET overwrites",
                    redisCommand(context, "SET %s other", key),
                    REDIS_REPLY_STATUS, "OK", 0) < 0) goto done;
    /* 5. 再 GET，要求实际读到 other，确认覆盖确实生效。 */
    if (check_reply(context, "GET overwritten value",
                    redisCommand(context, "GET %s", key),
                    REDIS_REPLY_STRING, "other", 0) < 0) goto done;
    /* 6. 发送 DEL，要求整数回复 1，表示实际删除了这个测试 key。 */
    if (check_reply(context, "DEL", redisCommand(context, "DEL %s", key),
                    REDIS_REPLY_INTEGER, NULL, 1) < 0) goto done;
    /* 7. 删除后再 GET，要求空值回复 NIL，确认已经读不到这个 key。 */
    if (check_reply(context, "missing GET",
                    redisCommand(context, "GET %s", key),
                    REDIS_REPLY_NIL, NULL, 0) < 0) goto done;
    /* 8. 故意发送未知命令 BOGUS，要求错误回复 ERR unknown command。
     * 这里 PASS 的意思是“服务器正确拒绝未知命令”，不是支持这个命令。
     */
    if (check_reply(context, "unknown command",
                    redisCommand(context, "BOGUS"),
                    REDIS_REPLY_ERROR, "ERR unknown command", 0) < 0) goto done;

    /* 只有全部检查通过才执行到这里。任一失败会 goto done，保留 result=1。 */
    result = 0;
done:
    /* 关闭连接并释放 hiredis 的连接资源，不会停止 kvstore 服务。
     * 失败时也会走到这里，但若尚未执行 DEL，测试 key 可能仍留在服务器中。
     */
    redisFree(context);
    return result;
}
