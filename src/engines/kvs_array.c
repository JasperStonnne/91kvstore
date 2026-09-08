#include"kvstore.h"








//考虑设计模式 这里可以用 singleton
kvs_array_t global_array={0};
/*
@return:<0,error;=0,success;>0 no exist;
*/

int kvs_array_create(kvs_array_t *inst){
    if(inst==NULL) return -1;
    if(inst->table){
    printf("table has been alloced");
    return -1;
    } 
    inst->table=kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
    if(!inst->table){
        return -1;
    }

    inst->total=0;
    return 0;
}
void kvs_array_destory(kvs_array_t *inst){

    if(!inst||!inst->table) return;
    for(int i=0;i<inst->total;i++){
        kvs_free(inst->table[i].key);
        kvs_free(inst->table[i].value);
        inst->table[i].key=NULL;
        inst->table[i].value=NULL;
    }
        kvs_free(inst->table);
        inst->table=NULL;
        inst->total=0;
        inst->idx=0;

}
/*
@return:<0,error;=0,success;>0,exit
*/

int kvs_array_set(kvs_array_t *inst, char *key, char *value) {
    if (inst == NULL || inst->table == NULL ||
        key == NULL || value == NULL) {
        return -1;
    }

    if (inst->total >= KVS_ARRAY_SIZE) {
        return -1;
    }

    if (kvs_array_get(inst, key) != NULL) {
        return 1;
    }

    size_t key_size = strlen(key) + 1;
    char *kcopy = kvs_malloc(key_size);
    if (kcopy == NULL) {
        return -2;
    }
    memcpy(kcopy, key, key_size);

    size_t value_size = strlen(value) + 1;
    char *kvalue = kvs_malloc(value_size);
    if (kvalue == NULL) {
        kvs_free(kcopy);
        return -2;
    }
    memcpy(kvalue, value, value_size);

    int i = 0;

    for (i = 0; i < inst->total; i++) {
        if (inst->table[i].key == NULL) {
            inst->table[i].key = kcopy;
            inst->table[i].value = kvalue;
            inst->total++;
            return 0;
        }
    }

    if (i == inst->total && i < KVS_ARRAY_SIZE) {
        inst->table[i].key = kcopy;
        inst->table[i].value = kvalue;
        inst->total++;
    }

    return 0;
}

char* kvs_array_get(kvs_array_t *inst,char *key){
    if(inst==NULL||key==NULL) return NULL;
    int i=0;
    for(i=0;i<inst->total;i++){
        if(inst->table[i].key==NULL){
            continue;
        }
        if(strcmp(inst->table[i].key,key)==0){
            return inst->table[i].value;
        }
    }
    
    return NULL; 
}

int kvs_array_del(kvs_array_t *inst,char *key){
    if(inst==NULL||key==NULL) return -1;

    int i=0;
    for( i=0;i<inst->total;i++){

        if(strcmp(inst->table[i].key,key)==0){

            kvs_free(inst->table[i].key);
            inst->table[i].key=NULL;

            kvs_free(inst->table[i].value);
            inst->table[i].value=NULL;

            inst->total--;
        if (i != inst->total) {
            inst->table[i] = inst->table[inst->total];
        }   

        inst->table[inst->total].key = NULL;
        inst->table[inst->total].value = NULL;            
        inst->idx=i;

            return 0;
        }

    }
    return 1;

}
/*
@return:<0,error;=0,success;>0 no exist;

*/
int kvs_array_mod(kvs_array_t *inst, char *key, char *value) {
    if (inst == NULL || key == NULL || value == NULL) {
        return -1;
    }

    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key == NULL) {
            continue;
        }

        if (strcmp(inst->table[i].key, key) == 0) {
            size_t value_size = strlen(value) + 1;

            char *kvalue = kvs_malloc(value_size);
            if (kvalue == NULL) {
                return -2;
            }

            memcpy(kvalue, value, value_size);

            kvs_free(inst->table[i].value);
            inst->table[i].value = kvalue;

            return 0;
        }
    }

    return 1;
}
/*
@return 0:exist,1:no exist

*/
int kvs_array_exist(kvs_array_t *inst, char *key){

    char *str=kvs_array_get(inst,key);
    if(!str){
        return 1;//不存在
    }
    return 0;//存在
}


int kvs_array_foreach(kvs_array_t *inst,kvs_visit_handler visitor,void *context){
    if(inst==NULL||inst->table==NULL||visitor==NULL){
        return -1;
    }
    for(int i=0;i<inst->total;i++){
        if(inst->table[i].key==NULL||inst->table[i].value==NULL){
            continue;
        }
        int ret=visitor(inst->table[i].key, inst->table[i].value,context);
        if(ret<0){
            return -1;
        }
    }
    return 0;
}