#define _GNU_SOURCE
#include "process_target.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static FILE *log_file;
static char log_text[4096];
static int resolve(const char *target, pid_t *pid)
{
    assert(ftruncate(fileno(log_file), 0) == 0);
    rewind(log_file);
    int result = kvs_resolve_process_target(target, pid, log_file);
    fflush(log_file);
    rewind(log_file);
    size_t length = fread(log_text, 1, sizeof(log_text) - 1, log_file);
    log_text[length] = '\0';
    return result;
}

int main(void)
{
    log_file = tmpfile();
    assert(log_file);
    pid_t target;
    for (size_t i = 0; i < 5; i++) {
        const char *invalid[] = {"", "0", "999999999999999999999999", "123abc", "abcdefghijklmnop"};
        assert(resolve(invalid[i], &target) < 0);
    }
    char numeric[32], old_name[16], name[16];
    snprintf(numeric, sizeof(numeric), "%ld", (long)getpid());
    assert(resolve(numeric, &target) == 0 && target == getpid());
    snprintf(numeric, sizeof(numeric), "000%ld", (long)getpid());
    assert(resolve(numeric, &target) == 0 && target == getpid());
    assert(prctl(PR_GET_NAME, old_name) == 0);
    snprintf(name, sizeof(name), "s41p-%010u", (unsigned int)getpid());
    assert(strlen(name) == 15);
    assert(prctl(PR_SET_NAME, name) == 0);
    assert(resolve(name, &target) < 0 && strstr(log_text, "not found")); /* skip self */
    int keep[2];
    assert(pipe(keep) == 0);
    pid_t children[3];
    for (int i = 0; i < 3; i++) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (children[i] == 0) {
            close(keep[1]);
            char byte;
            ssize_t count = read(keep[0], &byte, 1);
            _exit(count < 0 ? 1 : 0);
        }
        if (i == 0) {
            assert(resolve(name, &target) == 0 && target == children[0]);
            assert(strstr(log_text, "resolved process"));
            char prefix[16];
            memcpy(prefix, name, sizeof(prefix));
            prefix[14] = '\0';
            assert(resolve(prefix, &target) < 0 && strstr(log_text, "not found"));
            char wrong[16];
            snprintf(wrong, sizeof(wrong), "x%ld", (long)getpid());
            assert(resolve(wrong, &target) < 0 && strstr(log_text, "not found"));
        } else {
            assert(resolve(name, &target) < 0 && strstr(log_text, "ambiguous process"));
            for (int j = 0; j <= i; j++) {
                snprintf(numeric, sizeof(numeric), "%ld", (long)children[j]);
                assert(strstr(log_text, numeric));
            }
        }
    }
    close(keep[0]);close(keep[1]);
    for (int i = 0; i < 3; i++) {
        int status;
        assert(waitpid(children[i], &status, 0) == children[i]);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    assert(resolve(name, &target) < 0 && strstr(log_text, "not found"));
    assert(prctl(PR_SET_NAME, old_name) == 0);
    fclose(log_file);
    puts("[PASS] numeric/zero-prefixed PID, invalid targets, exact 15-byte comm/prefix rejection, skip-self, no match, unique match, all 2/3 ambiguous PIDs, child cleanup");
    return 0;
}
