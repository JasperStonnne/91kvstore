#include <stdio.h>

#include "persistence.h"

int main(void)
{
    char *tokens[] = {"SET", "name", "Jasper"};

    if (kvs_aof_open("test_appendonly.aof") < 0) {
        printf("open failed\n");
        return 1;
    }

    if (kvs_aof_append(tokens, 3) < 0) {
        printf("append failed\n");
        kvs_aof_close();
        return 1;
    }

    if (kvs_aof_close() < 0) {
        printf("close failed\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
