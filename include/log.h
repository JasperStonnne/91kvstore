#ifndef KVS_LOG_H
#define KVS_LOG_H

/* 数字越大，允许显示的运行细节越多；错误始终可见。 */
typedef enum {
    KVS_LOG_ERROR = 0,
    KVS_LOG_INFO,
    KVS_LOG_DEBUG
} kvs_log_level_t;

/* 服务启动时设置一次，后续网络和复制模块共用。 */
void kvs_log_set_level(kvs_log_level_t level);

/* 根据当前级别决定是否输出；格式参数用法与 printf 相同。 */
void kvs_log_message(kvs_log_level_t level, const char *format, ...);

#endif