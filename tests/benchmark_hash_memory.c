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
    kvs_hash_t hash={0};
    if(kvs_hash_create(&hash)!=0){
        fprintf(stderr,"failed to create hash\n");
        return 1;
}
    char key[64];
    char value[64];
    for(int i=0;i<entries;i++){
        snprintf(key,sizeof(key),"key-%d",i);
        snprintf(value,sizeof(value),"value-%d",i);
        if(kvs_hash_set(&hash,key,value)!=0){
            fprintf(stderr,"hash set failed at entry %d\n",i);
            kvs_hash_destory(&hash);
            return 1;
        }
    }
    printf("inserted nodes: %d\n", kvs_hash_count(&hash));
    wait_for_measurement("after insert");

    for (int i = 0; i < entries; i++) {
    snprintf(key, sizeof(key), "key-%d", i);

    if (kvs_hash_del(&hash, key) != 0) {
        fprintf(stderr, "delete failed at %d\n", i);
        kvs_hash_destory(&hash);
        return 1;
    }
}

    printf("remaining nodes: %d\n", kvs_hash_count(&hash));
    wait_for_measurement("after delete");

    kvs_hash_destory(&hash);

    wait_for_measurement("after destroy");

    return 0;
}