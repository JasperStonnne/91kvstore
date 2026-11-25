#include "process_target.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int decimal(const char *value)
{
    if (!value || !*value) return 0;
    for (const char *p = value; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

static int parse_pid(const char *value, pid_t *pid)
{
    if (!decimal(value)) return -1;
    errno = 0;
    char *end;
    long number = strtol(value, &end, 10);
    if (errno || *end || number <= 0 || number > INT_MAX) return -1;
    *pid = (pid_t)number;
    return 0;
}

int kvs_resolve_process_target(const char *target, pid_t *pid, FILE *diagnostic)
{
    if (!diagnostic) diagnostic = stderr;
    if (!target || !*target || !pid) {
        fprintf(diagnostic, "invalid empty process target or PID output\n");
        return -1;
    }
    if (target[0] >= '0' && target[0] <= '9') {
        if (parse_pid(target, pid) == 0) return 0;
        fprintf(diagnostic, "invalid decimal Primary PID: '%s'\n", target);
        return -1;
    }
    if (strlen(target) > 15) {
        fprintf(diagnostic, "process name exceeds Linux comm limit (15 bytes): '%s'\n", target);
        return -1;
    }
    DIR *proc = opendir("/proc");
    if (!proc) {
        fprintf(diagnostic, "cannot scan /proc: %s\n", strerror(errno));
        return -1;
    }
    size_t matches = 0;
    pid_t first = -1;
    int failed = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(proc);
        if (!entry) {
            if (errno) {
                fprintf(diagnostic, "cannot finish scanning /proc: %s\n", strerror(errno));
                failed = 1;
            }
            break;
        }
        pid_t candidate;
        if (parse_pid(entry->d_name, &candidate) < 0 || candidate == getpid()) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/comm", (long)candidate);
        FILE *file = fopen(path, "r");
        if (!file) {
            /* PID在扫描期间退出是正常竞争；无法读取其他进程时不能猜测唯一性。 */
            if (errno == ENOENT || errno == ESRCH) continue;
            fprintf(diagnostic, "cannot read %s: %s\n", path, strerror(errno));
            failed = 1;
            break;
        }
        char name[17];
        size_t length = fread(name, 1, sizeof(name) - 1, file);
        int error = ferror(file);
        int read_errno = errno;
        fclose(file);
        if (error) {
            if (read_errno == ENOENT || read_errno == ESRCH) continue;
            fprintf(diagnostic, "cannot read %s: %s\n", path, strerror(read_errno ? read_errno : EIO));
            failed = 1;
            break;
        }
        if (length && name[length - 1] == '\n') length--;
        name[length] = '\0';
        if (strcmp(name, target) != 0) continue;
        matches++;
        if (matches == 1) first = candidate;
        else if (matches == 2)
            fprintf(diagnostic, "ambiguous process '%s'; matching PIDs: %ld %ld", target, (long)first, (long)candidate);
        else
            fprintf(diagnostic, " %ld", (long)candidate);
    }
    closedir(proc);
    if (matches > 1) fputc('\n', diagnostic);
    if (failed) return -1;
    if (!matches) {
        fprintf(diagnostic, "process '%s' not found\n", target);
        return -1;
    }
    if (matches > 1) return -1;
    *pid = first;
    fprintf(diagnostic, "resolved process '%s' to PID %ld\n", target, (long)first);
    return 0;
}
