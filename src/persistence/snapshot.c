#include <stdio.h>
#include<unistd.h>
#include <errno.h>
#include<stdlib.h>
#include "kvstore.h"
#include "persistence.h"

#define PATH_MAX 128

#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif

#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif

#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif

#if ENABLE_SKIPLIST
extern kvs_skiplist_t global_skiplist;
#endif

typedef struct snapshot_write_context {
    FILE *fp;
    const char *command;
} snapshot_write_context_t;
static int snapshot_write_item(const char *key,const char *value,void *context) {
    if (key == NULL ||value == NULL ||context == NULL) {
        return -1;
    }
    snapshot_write_context_t *ctx=(snapshot_write_context_t *)context;
    if(ctx->fp==NULL||ctx->command==NULL){
        return -1;
    }
    int written=0;
    written=fprintf(ctx->fp,"%s %s %s\n",ctx->command,key,value);
    if(written<0){
        return -1;
    }
    return 0;
}
int kvs_snapshot_save(const char *path){
    if(path==NULL){
        return -1;
    }
    long long aof_offset=kvs_aof_get_offset();
    if(aof_offset<0){
        return -1;
    }
    char temp_path[PATH_MAX];
    int path_length=snprintf(temp_path,sizeof(temp_path),"%s.tmp",path);
    if (path_length < 0 ||
    (size_t)path_length >= sizeof(temp_path)) {
    return -1;
}
    FILE *fp=fopen(temp_path,"w");
    if(fp==NULL){
        return -1;
    }
    snapshot_write_context_t ctx={
        .fp=fp,
        .command="SET"
    };
    int result=0;
    int offset_written=fprintf(fp,"AOF_OFFSET %lld\n",aof_offset);
    if(offset_written<0){
        result=-1;
    }
#if ENABLE_ARRAY
    if(result==0){
    if(kvs_array_foreach(&global_array,snapshot_write_item,&ctx)<0){
        result=-1;
    }
    }
#endif
#if ENABLE_RBTREE
 if (result == 0) {
        ctx.command = "RSET";
        if (kvs_rbtree_foreach(
                &global_rbtree,
                snapshot_write_item,
                &ctx) < 0) {
            result = -1;
        }
    }
#endif
#if ENABLE_HASH
    if (result == 0) {
        ctx.command = "HSET";

        if (kvs_hash_foreach(
                &global_hash,
                snapshot_write_item,
                &ctx) < 0) {
            result = -1;
        }
    }
#endif
#if ENABLE_SKIPLIST
    if (result == 0) {
        ctx.command = "SSET";

        if (kvs_skiplist_foreach(
                &global_skiplist,
                snapshot_write_item,
                &ctx) < 0) {
            result = -1;
        }
    }
#endif
    if (result == 0) {
        int flush_result = fflush(fp);

        if (flush_result != 0) {
            result = -1;
        }
    }
    if(result==0){
        int sync_result=fsync(fileno(fp));
        if(sync_result!=0){
            result= -1;
        }
    }
    int close_result=fclose(fp);
    if(close_result!=0){
        result=-1;
    }
    if (result < 0) {
        unlink(temp_path);
        return -1;
    }
    int rename_result=rename(temp_path,path);
    if(rename_result!=0){
        unlink(temp_path);
        return -1;
    }
    return 0;
}
long long kvs_snapshot_load(const char* path,aof_replay_handler handler){
    if(path==NULL||handler==NULL){
        return -1;
    }
    FILE *fp=fopen(path,"r");
   if (fp == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
    return -1;
}
    char *line=NULL;//保存读到的第一行
    size_t capacity=0;//getline 为line分配的空间大小
    ssize_t line_length=getline(&line,&capacity,fp);
    if(line_length==-1){
        free(line);
        fclose(fp);
        return -1;
    }
    long long aof_offset=-1;
    int parsed=sscanf(line,"AOF_OFFSET %lld",&aof_offset);
    if(parsed!=1||aof_offset<0){
        free(line);
        fclose(fp);
        return -1;
    }
    int load_result=0;
    while((line_length=getline(&line,&capacity,fp))!=-1){
        while(line_length>0&&(line[line_length-1]=='\n'||line[line_length-1]=='\r')){
            line[--line_length]='\0';
        }
        if (line_length == 0) {
            continue;
    }
        char response[1024]={0};
        int response_length=handler(line,(int)line_length,response);
        if(response_length<0||strcmp(response, "OK\r\n") != 0){
            load_result=-1;
            break;
        }

    }
    if(ferror(fp)){
        load_result=-1;
    }
    free(line);
    if(fclose(fp)!=0){
        load_result=-1;
    }
    if(load_result<0){
        return -1;
    }
    return aof_offset;
}