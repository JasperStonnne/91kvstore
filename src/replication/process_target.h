#ifndef KVS_PROCESS_TARGET_H
#define KVS_PROCESS_TARGET_H
#include <stdio.h>
#include <sys/types.h>

/* 纯十进制PID或精确Linux comm名；失败仅诊断，不选择歧义目标。 */
int kvs_resolve_process_target(const char *target, pid_t *pid, FILE *diagnostic);
#endif
