//20 万节点时，两种分配方式的 VSZ/RSS 分别是多少？
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "kvstore.h"

void *kvs_malloc(size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    free(ptr);
}
static void wait_for_measurement(const char *phase) {
    long pid = (long)getpid();


    printf("Phase: %s\n", phase);

    printf("PID: %ld\n", pid);


    printf("press Enter to continue...\n");

    fflush(stdout);
    getchar();
}
int main(){
    const int entries=200000;
    kvs_skiplist_t skiplist={0};
    if(kvs_skiplist_create(&skiplist)!=0){
        fprintf(stderr,"failed to create skiplist\n");
        return 1;
}
    char key[64];
    char value[64];
    for(int i=0;i<entries;i++){
        snprintf(key,sizeof(key),"key-%d",i);
        snprintf(value,sizeof(value),"value-%d",i);
        if(kvs_skiplist_set(&skiplist,key,value)!=0){
            fprintf(stderr,"skiplist set failed at entry %d\n",i);
            kvs_skiplist_destory(&skiplist);
            return 1;
        }
    }
    printf("inserted nodes: %d\n", entries);
    wait_for_measurement("after insert");

    for (int i = 0; i < entries; i++) {
    snprintf(key, sizeof(key), "key-%d", i);

    if (kvs_skiplist_del(&skiplist, key) != 0) {
        fprintf(stderr, "delete failed at %d\n", i);
        kvs_skiplist_destory(&skiplist);
        return 1;
    }
}

   printf("remaining nodes: 0\n");
    wait_for_measurement("after delete");

    kvs_skiplist_destory(&skiplist);

    wait_for_measurement("after destroy");

    return 0;
}