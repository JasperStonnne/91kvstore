#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int check_reply(
    redisContext *context, const char *name, redisReply *reply,
    int expected_type, const char *expected_text, long long expected_integer)
{
    int ok = reply != NULL && reply->type == expected_type;

    if (ok && expected_text != NULL) {
        ok = reply->str != NULL &&
             reply->len == strlen(expected_text) &&
             memcmp(reply->str, expected_text, reply->len) == 0;
    }
    if (ok && expected_type == REDIS_REPLY_INTEGER) {
        ok = reply->integer == expected_integer;
    }

    if (!ok) {
        fprintf(stderr, "[FAIL] %s: reply type=%d, connection error=%s\n",
                name, reply != NULL ? reply->type : -1, context->errstr);
    } else {
        printf("[PASS] %s\n", name);
    }

    if (reply != NULL) freeReplyObject(reply);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 2;
    }

    redisContext *context = redisConnect("127.0.0.1", atoi(argv[1]));
    if (context == NULL || context->err) {
        fprintf(stderr, "hiredis connection failed: %s\n",
                context != NULL ? context->errstr : "out of memory");
        if (context != NULL) redisFree(context);
        return 1;
    }

    char key[64];
    snprintf(key, sizeof(key), "hiredis-probe-%ld", (long)getpid());
    const char value[] = "a b\r\nc";
    int result = 1;

    if (check_reply(context, "PING", redisCommand(context, "PING"),
                    REDIS_REPLY_STATUS, "PONG", 0) < 0) goto done;
    if (check_reply(context, "SET", redisCommand(
                        context, "SET %s %b", key, value, sizeof(value) - 1),
                    REDIS_REPLY_STATUS, "OK", 0) < 0) goto done;
    if (check_reply(context, "duplicate SET",
                    redisCommand(context, "SET %s other", key),
                    REDIS_REPLY_ERROR, "ERR key already exists", 0) < 0) goto done;
    if (check_reply(context, "GET with spaces and CRLF",
                    redisCommand(context, "GET %s", key),
                    REDIS_REPLY_STRING, value, 0) < 0) goto done;
    if (check_reply(context, "DEL", redisCommand(context, "DEL %s", key),
                    REDIS_REPLY_INTEGER, NULL, 1) < 0) goto done;
    if (check_reply(context, "missing GET",
                    redisCommand(context, "GET %s", key),
                    REDIS_REPLY_NIL, NULL, 0) < 0) goto done;
    if (check_reply(context, "unknown command",
                    redisCommand(context, "BOGUS"),
                    REDIS_REPLY_ERROR, "ERR unknown command", 0) < 0) goto done;

    result = 0;
done:
    redisFree(context);
    return result;
}