#include "log.h"

#include <stdarg.h>
#include <stdio.h>

/* 未显式设置时沿用当前正常输出的习惯：显示 INFO，不显示 DEBUG。 */
static kvs_log_level_t current_level = KVS_LOG_INFO;

void kvs_log_set_level(kvs_log_level_t level)
{
    current_level = level;
}

void kvs_log_message(kvs_log_level_t level, const char *format, ...)
{
    if (format == NULL ||
        level < KVS_LOG_ERROR ||
        level > KVS_LOG_DEBUG ||
        level > current_level) {
        return;
    }

    /* 错误写 stderr；正常运行信息和调试信息写 stdout。 */
    FILE *stream = level == KVS_LOG_ERROR ? stderr : stdout;
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stream, format, arguments);
    va_end(arguments);

    /* 测试把输出重定向到文件；及时刷新才能看到启动/失败日志。 */
    fflush(stream);
}