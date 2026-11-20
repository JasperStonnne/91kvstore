#ifndef KVS_CAPTURE_SERVER_H
#define KVS_CAPTURE_SERVER_H
#include <signal.h>
#include <sys/types.h>

/* 一条活动会话；调用者拥有 BPF 对象，返回后由调用者卸载。 */
int capture_server_run(const char *endpoint, pid_t primary_pid,
                        const volatile sig_atomic_t *stopping);
#endif
