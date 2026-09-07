

#include"kvstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include<string.h>

kvs_skiplist_t global_skiplist;
#ifndef KVS_SKIPLIST_USE_MEMORY_POOL
#define KVS_SKIPLIST_USE_MEMORY_POOL 1
#endif

#define SKIPLIST_NODE_BLOCKS_PER_CHUNK 1024

static kvs_skiplist_node_t *skiplist_node_storage_alloc(kvs_skiplist_t *inst,bool use_node_pool){
    (void)inst;
    (void)use_node_pool;
#if KVS_SKIPLIST_USE_MEMORY_POOL
    if(use_node_pool){
        return (kvs_skiplist_node_t *)memory_pool_alloc(&inst->node_pool);
    }
#endif
    return (kvs_skiplist_node_t *)kvs_malloc(sizeof(kvs_skiplist_node_t));
}
static void skiplist_node_storage_free(kvs_skiplist_t *inst,kvs_skiplist_node_t *node,bool use_node_pool){
    (void)inst;
    (void)node;
    (void)use_node_pool;
    if(node==NULL){
        return;
    }
#if KVS_SKIPLIST_USE_MEMORY_POOL
    if(use_node_pool){
        memory_pool_free(&inst->node_pool,node);
        return;
    }
#endif
    kvs_free(node);
}
static kvs_skiplist_node_t* skiplist_create_node(kvs_skiplist_t *inst,int level, const char *key, const char* value,bool use_node_pool) {//改成const char* 传入字符串不应该修改
    if (inst == NULL ||key == NULL ||value == NULL ||level < 0 ||level > KVS_SKIPLIST_MAX_LEVEL) {return NULL;}
    kvs_skiplist_node_t *newNode =skiplist_node_storage_alloc(inst,use_node_pool);
    if(newNode==NULL){
        return NULL;
    }
    newNode->key = kvs_malloc(strlen(key)+1);
    if(newNode->key==NULL){
        skiplist_node_storage_free(inst,newNode,use_node_pool);
        return NULL;
    }
    strcpy(newNode->key,key);

    newNode->value = kvs_malloc(strlen(value)+1);
    if(newNode->value==NULL){
        kvs_free(newNode->key);
        skiplist_node_storage_free(inst,newNode,use_node_pool);
        return NULL;
    }

    strcpy(newNode->value,value);

    newNode->forward = (kvs_skiplist_node_t**)kvs_malloc((level + 1) * sizeof(kvs_skiplist_node_t*));
    if(newNode->forward==NULL){
        kvs_free(newNode->key);
        kvs_free(newNode->value);
        skiplist_node_storage_free(inst,newNode,use_node_pool);
        return NULL;

    }
    // 初始化这个节点拥有的全部 forward 指针
    for (int i = 0; i <= level; i++) {
    newNode->forward[i] = NULL;
    }
    return newNode;
}

int  kvs_skiplist_create(kvs_skiplist_t *inst) {
    kvs_skiplist_t* skipList =inst;
    if(skipList==NULL){
        return -1;
    }
#if KVS_SKIPLIST_USE_MEMORY_POOL
    if (memory_pool_init(&skipList->node_pool,sizeof(kvs_skiplist_node_t),SKIPLIST_NODE_BLOCKS_PER_CHUNK) != 0) {
    return -1;
    }
#endif
    skipList->level = 0;

    skipList->header = skiplist_create_node(skipList,KVS_SKIPLIST_MAX_LEVEL,"","",false); //头节点不保存业务数据 所以key和value使用空字符串
    if(skipList->header==NULL){
#if KVS_SKIPLIST_USE_MEMORY_POOL
        memory_pool_destory(&skipList->node_pool);
#endif
        return -1;
    }

    for (int i = 0; i <= KVS_SKIPLIST_MAX_LEVEL; ++i) {
        skipList->header->forward[i] = NULL;
    }

   return 0;
}
void kvs_skiplist_destory(kvs_skiplist_t *inst){

    if(inst==NULL||inst->header==NULL){
        return;
    }
    kvs_skiplist_node_t *current=inst->header->forward[0];
    while(current!=NULL){
    kvs_skiplist_node_t *next=current->forward[0];

    kvs_free(current->key);
    kvs_free(current->value);
    kvs_free(current->forward);
    skiplist_node_storage_free(inst,current,true);
    current=next;
    }
    kvs_free(inst->header->key);
    kvs_free(inst->header->value);
    kvs_free(inst->header->forward);
    kvs_free(inst->header);
#if KVS_SKIPLIST_USE_MEMORY_POOL
    memory_pool_destory(&inst->node_pool);
#endif
    inst->header = NULL;
    inst->level = 0;

}
static int skiplist_random_level() {
   int level = 0;
   while (rand() < RAND_MAX / 2 && level < KVS_SKIPLIST_MAX_LEVEL)
      level++;
   return level;
}

int kvs_skiplist_set(kvs_skiplist_t* skipList,char *key,char* value) {
    if(skipList==NULL||key==NULL||value==NULL||skipList->header==NULL){
        return -1;
    }
   kvs_skiplist_node_t* update[KVS_SKIPLIST_MAX_LEVEL + 1];
   kvs_skiplist_node_t* current = skipList->header;

   for (int i = skipList->level; i >= 0; --i) {
      while (current->forward[i] != NULL && strcmp(current->forward[i]->key , key)<0){
         current = current->forward[i];

   }
        update[i] = current;
   }
   current = current->forward[0];

   if (current == NULL || strcmp(current->key,key)!=0) {
      int level = skiplist_random_level();

      if (level > skipList->level) {
         for (int i = skipList->level + 1; i <= level; ++i)
            update[i] = skipList->header;
    }
      kvs_skiplist_node_t* newNode = skiplist_create_node(skipList,level,key,value,true);
      if(newNode==NULL){
        return -2;
      }
      if (level > skipList->level) {
        skipList->level = level;
    }
      for (int i = 0; i <= level; ++i) {
         newNode->forward[i] = update[i]->forward[i];
         update[i]->forward[i] = newNode;
      }
      return 0 ;
   } else {
       return 1;
   }
}

void display(kvs_skiplist_t* skipList) {
    printf("Skip List:\n");

    for (int i = 0; i <= skipList->level; ++i) {
        kvs_skiplist_node_t* node = skipList->header->forward[i];
        printf("Level %d: ", i);

        while (node != NULL) {
            printf("%s ", node->key);
            node = node->forward[i];
        }

        printf("\n");
    }
}

static kvs_skiplist_node_t *skiplist_search_node(kvs_skiplist_t* skipList, const char* key) {
    if(skipList==NULL||key==NULL){
        return NULL;
    }
    kvs_skiplist_node_t* current = skipList->header;

    for (int i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && strcmp(current->forward[i]->key,key)<0){
            current = current->forward[i];
        }
    }

    current = current -> forward[0];

    if(current!=NULL && strcmp(current -> key,key)==0){
        return current;
    }
    return NULL;
}
    char *kvs_skiplist_get(kvs_skiplist_t *inst,char *key ){
        if(inst==NULL||key==NULL){
            return NULL;
        }
        kvs_skiplist_node_t *node=skiplist_search_node(inst,key);
        if(node==NULL){
            return NULL;
        }
        return node->value;
    }
int kvs_skiplist_del(kvs_skiplist_t *skipList, char *key){

    if(skipList==NULL||key==NULL||skipList->header==NULL){
        return -1;
    }
    kvs_skiplist_node_t *update[KVS_SKIPLIST_MAX_LEVEL+1];
    kvs_skiplist_node_t *current=skipList->header;
    for(int i=skipList->level;i>=0;--i){
        while(current->forward[i]!=NULL&&strcmp(current->forward[i]->key, key)<0){
            current=current->forward[i];
        }
        update[i]=current;
    }
    current = current->forward[0];
    if(current==NULL||strcmp(current->key,key)!=0){
        return 1;
    }
    for(int i=0;i<=skipList->level;++i){
        if(update[i]->forward[i]!=current){
            break;
        }
        update[i]->forward[i]=current->forward[i];
    }
    while(skipList->level>0&&skipList->header->forward[skipList->level]==NULL){
        skipList->level--;
    }
    kvs_free(current->forward);
    kvs_free(current->value);
    kvs_free(current->key);
   skiplist_node_storage_free(skipList,current,true);

    return 0;
}
/*
return:0-success, -1-error -2内存分配失败 1-no exist


*/
int kvs_skiplist_mod(kvs_skiplist_t *skipList, char *key, char *newValue){
    if(skipList==NULL||key==NULL||newValue==NULL||skipList->header==NULL){
        return -1;
    }
    kvs_skiplist_node_t *node=skiplist_search_node(skipList,key);
    if(node==NULL){
        return 1;
    }
    char *valueCopy=kvs_malloc(strlen(newValue)+1);
    if( valueCopy==NULL){
        return -2;
    }
    strcpy(valueCopy,newValue);
    kvs_free(node->value);
    node->value=valueCopy;

    return 0;
}

int kvs_skiplist_exist(kvs_skiplist_t *skipList, char *key){
    if(skipList==NULL||key==NULL||skipList->header==NULL){
        return -1;
    }
    kvs_skiplist_node_t *node=skiplist_search_node(skipList,key);
    if(node!=NULL){
        return 0;
    }
    return 1;
}
int kvs_skiplist_foreach(
    kvs_skiplist_t *inst,
    kvs_visit_handler visitor,
    void *context
) {
    if (inst == NULL ||
        inst->header == NULL ||
        visitor == NULL) {
        return -1;
    }

    kvs_skiplist_node_t *node =
        inst->header->forward[0];

    while (node != NULL) {
        if (node->key != NULL &&
            node->value != NULL) {

            int ret = visitor(
                node->key,
                node->value,
                context
            );

            if (ret < 0) {
                return -1;
            }
        }

        node = node->forward[0];
    }

    return 0;
}
#if 0
static void check_test(int condition, const char *testName)
{
    if (condition) {
        printf("[PASS] %s\n", testName);
    } else {
        printf("[FAIL] %s\n", testName);
    }
}


int main(void)
{
    kvs_skiplist_t skipListInstance;
    kvs_skiplist_t *skipList = &skipListInstance;

    int result = kvs_skiplist_create(skipList);
    check_test(result == 0, "create skiplist");

    if (result != 0) {
        return 1;
    }

    /* 测试 set */
    result = kvs_skiplist_set(skipList, "Dad", "Jasper");
    check_test(result == 0, "set new key");

    result = kvs_skiplist_set(skipList, "Dad", "Sao");
    check_test(result == 1, "reject duplicate key");

    result = kvs_skiplist_set(skipList, "Teacher", "King");
    check_test(result == 0, "set Teacher");

    result = kvs_skiplist_set(skipList, "Name", "Alice");
    check_test(result == 0, "set Name");

    result = kvs_skiplist_set(skipList, "User", "Bob");
    check_test(result == 0, "set User");

    /* 测试 get */
    char *value = kvs_skiplist_get(skipList, "Dad");

    check_test(
        value != NULL && strcmp(value, "Jasper") == 0,
        "get existing key"
    );

    value = kvs_skiplist_get(skipList, "Missing");
    check_test(value == NULL, "get missing key");

    /* 测试 exist */
    result = kvs_skiplist_exist(skipList, "Dad");
    check_test(result == 0, "existing key exists");

    result = kvs_skiplist_exist(skipList, "Missing");
    check_test(result == 1, "missing key does not exist");

    /* 测试 mod */
    result = kvs_skiplist_mod(skipList, "Dad", "Sao");
    check_test(result == 0, "modify existing key");

    value = kvs_skiplist_get(skipList, "Dad");

    check_test(
        value != NULL && strcmp(value, "Sao") == 0,
        "get modified value"
    );

    result = kvs_skiplist_mod(skipList, "Missing", "Value");
    check_test(result == 1, "modify missing key");

    /* 测试 del */
    result = kvs_skiplist_del(skipList, "Dad");
    check_test(result == 0, "delete existing key");

    result = kvs_skiplist_exist(skipList, "Dad");
    check_test(result == 1, "deleted key does not exist");

    result = kvs_skiplist_del(skipList, "Missing");
    check_test(result == 1, "delete missing key");

    /* 查看删除后的跳表内容 */
    display(skipList);

    /* 测试销毁 */
    kvs_skiplist_destory(skipList);

    check_test(
        skipList->header == NULL && skipList->level == 0,
        "destroy skiplist"
    );

    return 0;
}

#endif


