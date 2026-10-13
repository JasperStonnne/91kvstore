//kvstore.c

#include"kvstore.h"
#include "persistence.h"
#include "replication.h"
#include "server.h"
#include "protocol.h"
#include "config.h"
#include "log.h"
#include <string.h>
#include<errno.h>
#include<limits.h>
#include <stdint.h>

/* 只限制客户端的一条 RESP 命令；不改变 AOF/Snapshot/复制解析。 */
#define KVS_MAX_CLIENT_COMMAND_BYTES (1024u * 1024u)

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

void *kvs_malloc(size_t size){


    return malloc(size);
}

void kvs_free(void *ptr){

    return free(ptr);
}

const char *command[]={
    "SET","GET","DEL","MOD","EXIST",
    "RSET","RGET","RDEL","RMOD","REXIST",
    "HSET","HGET","HDEL","HMOD","HEXIST",
    "SSET","SGET","SDEL","SMOD","SEXIST"
};
enum{
    KVS_CMD_START=0,
    //array
    KVS_CMD_SET=KVS_CMD_START,//0
    KVS_CMD_GET,//1
    KVS_CMD_DEL,//2
    KVS_CMD_MOD,//3
    KVS_CMD_EXIST,//4
    //与上面 command 对应的 顺序不能变啊
    //rbtree
    KVS_CMD_RSET,
    KVS_CMD_RGET,//1
    KVS_CMD_RDEL,//2
    KVS_CMD_RMOD,//3
    KVS_CMD_REXIST,//4
    //hash
    KVS_CMD_HSET,
    KVS_CMD_HGET,//1
    KVS_CMD_HDEL,//2
    KVS_CMD_HMOD,//3
    KVS_CMD_HEXIST,//4
    //skiplist
    KVS_CMD_SSET,
    KVS_CMD_SGET,//1
    KVS_CMD_SDEL,//2
    KVS_CMD_SMOD,//3
    KVS_CMD_SEXIST,//4
    KVS_CMD_COUNT,
};

/* 命令的业务结果，不是发给客户端的 RESP 字节。 */
typedef enum {
    KVS_EXEC_FAILED = -1,
    KVS_EXEC_OK = 0,
    KVS_EXEC_ALREADY_EXISTS,
    KVS_EXEC_NOT_FOUND,
    KVS_EXEC_FOUND
} kvs_exec_status_t;

const char *response[]={


};

static kvs_role_t kvs_server_role = KVS_ROLE_STANDALONE;

static int kvs_is_write_command(int cmd){
    if(cmd<KVS_CMD_START||cmd>=KVS_CMD_COUNT){
        return 0;
    }
    switch (cmd%5)
    {
    case 0:
    case 2:
    case 3:
        return 1;
    default:
        return 0;
    }
}


/* 将解析出的字段复制为旧命令执行器使用的 C 字符串。 */
static int kvs_fields_to_tokens(
    const kvs_slice_t *fields,
    size_t field_count,
    char **tokens,
    size_t token_capacity)
{
    if (fields == NULL || tokens == NULL ||
        field_count == 0 || field_count > token_capacity) {
        return -1;
    }

    for (size_t i = 0; i < field_count; i++) {
        /* 旧执行器使用 strcmp 等函数，目前不能处理字段内的 NUL。 */
        if (fields[i].data == NULL ||
            fields[i].length == SIZE_MAX ||
            memchr(fields[i].data, '\0', fields[i].length) != NULL) {
            goto fail;
        }

        tokens[i] = malloc(fields[i].length + 1);
        if (tokens[i] == NULL) goto fail;

        memcpy(tokens[i], fields[i].data, fields[i].length);
        tokens[i][fields[i].length] = '\0';
    }
    return 0;

fail:
    for (size_t i = 0; i < field_count; i++) {
        free(tokens[i]);
        tokens[i] = NULL;
    }
    return -1;
}

/* 根据读取命令，从对应的存储引擎中取 value。 */
static char *kvs_get_value(int cmd, char *key)
{
    if (key == NULL) return NULL;

    switch (cmd) {
#if ENABLE_ARRAY
    case KVS_CMD_GET:
        return kvs_array_get(&global_array, key);
#endif
#if ENABLE_RBTREE
    case KVS_CMD_RGET:
        return kvs_rbtree_get(&global_rbtree, key);
#endif
#if ENABLE_HASH
    case KVS_CMD_HGET:
        return kvs_hash_get(&global_hash, key);
#endif
#if ENABLE_SKIPLIST
    case KVS_CMD_SGET:
        return kvs_skiplist_get(&global_skiplist, key);
#endif
    default:
        return NULL;
    }
}

//SET Key Value
//tokens[0]:SET
//tokens[1]:Key
//tokens[2]:Value
int kvs_filter_protocol(
    char **tokens,
    int count,
    char *response,
    int response_capacity,
    kvs_command_source_t source,
    kvs_exec_status_t *status)
{

    if (tokens == NULL || count <= 0 || tokens[0] == NULL || response == NULL||response_capacity <= 0){
        if (status) *status = KVS_EXEC_FAILED;
        return -1;
    }

    int cmd= KVS_CMD_START;
    for(cmd=KVS_CMD_START;cmd<KVS_CMD_COUNT;cmd++){
        if(strcmp(tokens[0],command[cmd])==0){
            break;
        }
    }

    if (cmd == KVS_CMD_COUNT) {
    return snprintf(response, (size_t)response_capacity, "ERROR unknown command\r\n");
    }

    if(kvs_server_role==KVS_ROLE_REPLICA&&source==KVS_COMMAND_SOURCE_CLIENT&&kvs_is_write_command(cmd)){
        return snprintf(response, (size_t)response_capacity,"READONLY replica does not accept client writes\r\n");
    }

    int length=0;
    int ret=0;
    char *key=tokens[1];
    char *value=tokens[2];
    switch(cmd){
#if ENABLE_ARRAY
    case KVS_CMD_SET:
        ret=kvs_array_set(&global_array,key,value);
        if(ret<0){
            length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"EXIST\r\n");
        }

        break;
    case KVS_CMD_GET:{
    char *result = kvs_get_value(cmd, key);
    if(result==NULL){
        length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
    }else {
        length=snprintf(response, (size_t)response_capacity,"%s\r\n",result);
    }
        break;
}
    case KVS_CMD_DEL:
        ret=kvs_array_del(&global_array,key);
        if(ret<0){
           length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
    case KVS_CMD_MOD:
        ret=kvs_array_mod(&global_array,key,value);
        if(ret<0){
           length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
    case KVS_CMD_EXIST:
        ret=kvs_array_exist(&global_array,key);
        if(ret==0){
        length=snprintf(response, (size_t)response_capacity,"EXIST\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
#endif
#if ENABLE_RBTREE
//rbtree
    case KVS_CMD_RSET:
        ret=kvs_rbtree_set(&global_rbtree,key,value);
        if(ret<0){
            length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"EXIST\r\n");
        }

        break;
    case KVS_CMD_RGET:{
    char *result = kvs_get_value(cmd, key);
    if(result==NULL){
        length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
    }else {
        length=snprintf(response, (size_t)response_capacity,"%s\r\n",result);
    }
        break;
}
    case KVS_CMD_RDEL:
        ret=kvs_rbtree_del(&global_rbtree,key);
        if(ret<0){
           length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
    case KVS_CMD_RMOD:
        ret=kvs_rbtree_mod(&global_rbtree,key,value);
        if(ret<0){
           length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
    case KVS_CMD_REXIST:
        ret=kvs_rbtree_exist(&global_rbtree,key);
        if(ret==0){
        length=snprintf(response, (size_t)response_capacity,"EXIST\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
#endif
#if ENABLE_HASH
    case KVS_CMD_HSET:
        ret=kvs_hash_set(&global_hash,key,value);
        if(ret<0){
            length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"EXIST\r\n");
        }

        break;
    case KVS_CMD_HGET:{
    char *result = kvs_get_value(cmd, key);
    if(result==NULL){
        length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
    }else {
        length=snprintf(response, (size_t)response_capacity,"%s\r\n",result);
    }
        break;
}
    case KVS_CMD_HDEL:
        ret=kvs_hash_del(&global_hash,key);
        if(ret<0){
           length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
    case KVS_CMD_HMOD:
        ret=kvs_hash_mod(&global_hash,key,value);
        if(ret<0){
           length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
    case KVS_CMD_HEXIST:
        ret=kvs_hash_exist(&global_hash,key);
        if(ret==0){
        length=snprintf(response, (size_t)response_capacity,"EXIST\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
#endif
#if ENABLE_SKIPLIST
    case KVS_CMD_SSET:
        ret=kvs_skiplist_set(&global_skiplist,key,value);
        if(ret<0){
            length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"EXIST\r\n");
        }

        break;
    case KVS_CMD_SGET:{
    char *result = kvs_get_value(cmd, key);
    if(result==NULL){
        length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
    }else {
        length=snprintf(response, (size_t)response_capacity,"%s\r\n",result);
    }
        break;
}
    case KVS_CMD_SDEL:
        ret=kvs_skiplist_del(&global_skiplist,key);
        if(ret<0){
           length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
    case KVS_CMD_SMOD:
        ret=kvs_skiplist_mod(&global_skiplist,key,value);
        if(ret<0){
           length=snprintf(response, (size_t)response_capacity,"ERROR\r\n");
        }else if(ret==0){
            length=snprintf(response, (size_t)response_capacity,"OK\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;
    case KVS_CMD_SEXIST:
        ret=kvs_skiplist_exist(&global_skiplist,key);
        if(ret==0){
        length=snprintf(response, (size_t)response_capacity,"EXIST\r\n");
        }else{
            length=snprintf(response, (size_t)response_capacity,"NO EXIST\r\n");
        }
        break;

#endif

    default:
        assert(0);
    }

    if (status != NULL) {
        if (ret < 0) {
            *status = KVS_EXEC_FAILED;
        } else {
            switch (cmd) {
            case KVS_CMD_SET:
            case KVS_CMD_RSET:
            case KVS_CMD_HSET:
            case KVS_CMD_SSET:
                *status = ret == 0
                    ? KVS_EXEC_OK : KVS_EXEC_ALREADY_EXISTS;
                break;

            case KVS_CMD_DEL:
            case KVS_CMD_RDEL:
            case KVS_CMD_HDEL:
            case KVS_CMD_SDEL:
            case KVS_CMD_MOD:
            case KVS_CMD_RMOD:
            case KVS_CMD_HMOD:
            case KVS_CMD_SMOD:
                *status = ret == 0
                    ? KVS_EXEC_OK : KVS_EXEC_NOT_FOUND;
                break;

            case KVS_CMD_EXIST:
            case KVS_CMD_REXIST:
            case KVS_CMD_HEXIST:
            case KVS_CMD_SEXIST:
                *status = ret == 0
                    ? KVS_EXEC_FOUND : KVS_EXEC_NOT_FOUND;
                break;

            default:
                break; /* GET 在上层单独处理。 */
            }
        }
    }

    if(ret==0&&kvs_is_write_command(cmd)&&source!=KVS_COMMAND_SOURCE_RECOVERY){
        int aof_ret=kvs_aof_append(tokens,count);
        if(aof_ret<0){
            fprintf(stderr,"failed to append AOF\n");
            exit(EXIT_FAILURE);
        }
    }


    return length;

}

/* 统一执行已经拆好的命令字段，供旧格式和新格式共用。 */
static int kvs_execute_tokens(
    char **tokens,
    int count,
    char *response,
    int response_capacity,
    kvs_command_source_t source,
    kvs_exec_status_t *status)
{
    if (tokens == NULL || count <= 0 ||
        tokens[0] == NULL || response == NULL || response_capacity <= 0) {
        return -1;
    }
        int response_length = kvs_filter_protocol(
        tokens, count, response, response_capacity, source, status
    );

    if (response_length < 0) {
        return -1;  /* 命令执行或回复生成失败 */
    }

    if (response_length >= response_capacity) {
        return -1;  /* 回复被截断；还需要留一个位置给 '\0' */
    }

    return response_length;
}




/* 将读取到的 value 编码成长度格式响应，空间不够时先扩容。 */
static int kvs_encode_value_to_output(
    kvs_output_buffer_t *output,
    const char *value)
{
    if (output == NULL || value == NULL) return -1;

    kvs_slice_t value_slice = {
        .data = value,
        .length = strlen(value)
    };

    size_t required = 0;
    if (kvs_encoded_value_response_size(&value_slice, &required) != 0 ||
        required > INT_MAX) {
        return -1;
    }

    if (kvs_output_buffer_ensure_space(output, (int)required) < 0) {
        return -1;
    }

    size_t encoded_bytes = 0;
    if (kvs_encode_value_response(
            &value_slice,
            output->data,
            (size_t)output->capacity,
            &encoded_bytes) != 0 ||
        encoded_bytes > INT_MAX) {
        return -1;
    }

    return (int)encoded_bytes;
}

/* 判断命令是否属于某个存储引擎的读取命令。 */
static int kvs_is_get_command(const char *name)
{
    if (name == NULL) return 0;

    return strcmp(name, "GET") == 0 ||
           strcmp(name, "RGET") == 0 ||
           strcmp(name, "HGET") == 0 ||
           strcmp(name, "SGET") == 0;
}

/* 将长度协议的字段交给现有命令执行器。 */
static int kvs_execute_fields(
    const kvs_slice_t *fields,
    size_t field_count,
    kvs_output_buffer_t *response,
    kvs_command_source_t source,
    kvs_exec_status_t *execution_status)
{
    char *tokens[KVS_MAX_TOKENS] = {0};
        if (execution_status != NULL) {
        *execution_status = KVS_EXEC_FAILED;
    }
        if (response == NULL ||
        response->data == NULL ||
        response->capacity <= 0 ||
        fields == NULL ||
        field_count == 0 ||
        field_count > KVS_MAX_TOKENS ||
        field_count > INT_MAX) {
        return -1;
    }

    for (size_t i = 0; i < field_count; i++) {
        if (fields[i].data == NULL || fields[i].length == SIZE_MAX) {
            return -1;
        }

        if (memchr(fields[i].data, '\0', fields[i].length) != NULL) {
            static const char reply[] =
                "-ERR NUL bytes are not supported\r\n";
            const size_t reply_length = sizeof(reply) - 1;

            if (source != KVS_COMMAND_SOURCE_CLIENT ||
                reply_length > (size_t)response->capacity) {
                return -1;
            }

            memcpy(response->data, reply, reply_length);
            return (int)reply_length;
        }
    }

    if (kvs_fields_to_tokens(
            fields, field_count, tokens, KVS_MAX_TOKENS) != 0) {
        return -1;
    }
        if (source == KVS_COMMAND_SOURCE_CLIENT &&
        strcmp(tokens[0], "PING") == 0) {
        static const char pong[] = "+PONG\r\n";
        static const char wrong_arity[] =
            "-ERR wrong number of arguments for PING\r\n";

        const char *reply = field_count == 1 ? pong : wrong_arity;
        size_t reply_length = field_count == 1
            ? sizeof(pong) - 1 : sizeof(wrong_arity) - 1;
        int result = -1;

        if (reply_length <= (size_t)response->capacity) {
            memcpy(response->data, reply, reply_length);
            result = (int)reply_length;
        }

        for (size_t i = 0; i < field_count; i++) {
            free(tokens[i]);
        }
        return result;
    }

        if (source == KVS_COMMAND_SOURCE_CLIENT &&
        strcmp(tokens[0], "SAVE") == 0) {
        static const char ok[] = "+OK\r\n";
        static const char wrong_arity[] =
            "-ERR wrong number of arguments for SAVE\r\n";
        static const char failed[] = "-ERR SAVE failed\r\n";

        const char *reply;
        size_t reply_length;
        int result = -1;

        if (field_count != 1) {
            reply = wrong_arity;
            reply_length = sizeof(wrong_arity) - 1;
        } else if (kvs_snapshot_save("snapshot.db", NULL) < 0) {
            reply = failed;
            reply_length = sizeof(failed) - 1;
        } else {
            reply = ok;
            reply_length = sizeof(ok) - 1;
        }

        if (reply_length <= (size_t)response->capacity) {
            memcpy(response->data, reply, reply_length);
            result = (int)reply_length;
        }

        for (size_t i = 0; i < field_count; i++) {
            free(tokens[i]);
        }
        return result;
    }

    int command_id = KVS_CMD_START;
    while (command_id < KVS_CMD_COUNT &&
           strcmp(tokens[0], command[command_id]) != 0) {
        command_id++;
    }

    if (command_id < KVS_CMD_COUNT) {
        /* 每组命令依次是 SET、GET、DEL、MOD、EXIST。 */
        size_t expected = (command_id % 5 == 0 || command_id % 5 == 3)
            ? 3 : 2;

        if (field_count != expected) {
            int written = -1;
            if (source == KVS_COMMAND_SOURCE_CLIENT) {
                written = snprintf(
                    response->data,
                    (size_t)response->capacity,
                    "-ERR wrong number of arguments for %s\r\n",
                    tokens[0]
                );
                if (written < 0 || written >= response->capacity) {
                    written = -1;
                }
            }

            for (size_t i = 0; i < field_count; i++) {
                free(tokens[i]);
            }
            return written;
        }
    }

    if (source == KVS_COMMAND_SOURCE_CLIENT &&
        command_id == KVS_CMD_COUNT) {
        static const char reply[] = "-ERR unknown command\r\n";
        const size_t reply_length = sizeof(reply) - 1;
        int result = -1;

        if (reply_length <= (size_t)response->capacity) {
            memcpy(response->data, reply, reply_length);
            result = (int)reply_length;
        }

        for (size_t i = 0; i < field_count; i++) {
            free(tokens[i]);
        }
        return result;
    }

        if (field_count == 2 && kvs_is_get_command(tokens[0])) {
        int command_id = KVS_CMD_START;

        while (command_id < KVS_CMD_COUNT &&
               strcmp(tokens[0], command[command_id]) != 0) {
            command_id++;
        }

        if (command_id < KVS_CMD_COUNT) {
            char *value = kvs_get_value(command_id, tokens[1]);
            int result;

        if (value == NULL) {
            static const char nil_reply[] = "$-1\r\n";
            const size_t nil_length = sizeof(nil_reply) - 1;

            if (nil_length > (size_t)response->capacity) {
                result = -1;
            } else {
                memcpy(response->data, nil_reply, nil_length);
                result = (int)nil_length;
            }
        } else {
                result = kvs_encode_value_to_output(response, value);
            }

            for (size_t i = 0; i < field_count; i++) {
                free(tokens[i]);
            }

            return result;
        }
    }

    kvs_exec_status_t exec_status = KVS_EXEC_FAILED;
    int result = kvs_execute_tokens(
        tokens,
        (int)field_count,
        response->data,
        response->capacity,
        source,
        &exec_status
    );
    if (execution_status != NULL && result >= 0) {
    *execution_status = exec_status;
}
    if (result >= 0 &&
    source == KVS_COMMAND_SOURCE_CLIENT &&
    (command_id == KVS_CMD_SET ||
    command_id == KVS_CMD_RSET ||
    command_id == KVS_CMD_HSET ||
    command_id == KVS_CMD_SSET)) {
    static const char ok[] = "+OK\r\n";
    static const char exists[] = "-ERR key already exists\r\n";
    static const char failed[] = "-ERR SET failed\r\n";

    const char *reply = failed;
    size_t reply_length = sizeof(failed) - 1;

    if (exec_status == KVS_EXEC_OK) {
        reply = ok;
        reply_length = sizeof(ok) - 1;
    } else if (exec_status == KVS_EXEC_ALREADY_EXISTS) {
        reply = exists;
        reply_length = sizeof(exists) - 1;
    }

    if (reply_length > (size_t)response->capacity) {
        result = -1;
    } else {
        memcpy(response->data, reply, reply_length);
        result = (int)reply_length;
    }

    }

    if (result >= 0 &&
        source == KVS_COMMAND_SOURCE_CLIENT &&
        (command_id == KVS_CMD_DEL ||
         command_id == KVS_CMD_RDEL ||
         command_id == KVS_CMD_HDEL ||
         command_id == KVS_CMD_SDEL)) {
        static const char deleted[] = ":1\r\n";
        static const char missing[] = ":0\r\n";
        static const char failed[] = "-ERR DEL failed\r\n";

        const char *reply = failed;
        size_t reply_length = sizeof(failed) - 1;
        if (exec_status == KVS_EXEC_OK) {
            reply = deleted;
            reply_length = sizeof(deleted) - 1;
        } else if (exec_status == KVS_EXEC_NOT_FOUND) {
            reply = missing;
            reply_length = sizeof(missing) - 1;
        }

        if (reply_length > (size_t)response->capacity) {
            result = -1;
        } else {
            memcpy(response->data, reply, reply_length);
            result = (int)reply_length;
        }
    }

        if (result >= 0 &&
        source == KVS_COMMAND_SOURCE_CLIENT &&
        (command_id == KVS_CMD_MOD ||
         command_id == KVS_CMD_RMOD ||
         command_id == KVS_CMD_HMOD ||
         command_id == KVS_CMD_SMOD)) {
        static const char modified[] = "+OK\r\n";
        static const char missing[] = "-ERR key not found\r\n";
        static const char failed[] = "-ERR MOD failed\r\n";

        const char *reply = failed;
        size_t reply_length = sizeof(failed) - 1;
        if (exec_status == KVS_EXEC_OK) {
            reply = modified;
            reply_length = sizeof(modified) - 1;
        } else if (exec_status == KVS_EXEC_NOT_FOUND) {
            reply = missing;
            reply_length = sizeof(missing) - 1;
        }

        if (reply_length > (size_t)response->capacity) {
            result = -1;
        } else {
            memcpy(response->data, reply, reply_length);
            result = (int)reply_length;
        }
    }
    if (result >= 0 &&
        source == KVS_COMMAND_SOURCE_CLIENT &&
        (command_id == KVS_CMD_EXIST ||
         command_id == KVS_CMD_REXIST ||
         command_id == KVS_CMD_HEXIST ||
         command_id == KVS_CMD_SEXIST)) {
        static const char found[] = ":1\r\n";
        static const char missing[] = ":0\r\n";
        static const char failed[] = "-ERR EXIST failed\r\n";

        const char *reply = failed;
        size_t reply_length = sizeof(failed) - 1;
        if (exec_status == KVS_EXEC_FOUND) {
            reply = found;
            reply_length = sizeof(found) - 1;
        } else if (exec_status == KVS_EXEC_NOT_FOUND) {
            reply = missing;
            reply_length = sizeof(missing) - 1;
        }

        if (reply_length > (size_t)response->capacity) {
            result = -1;
        } else {
            memcpy(response->data, reply, reply_length);
            result = (int)reply_length;
        }
    }

    for (size_t i = 0; i < field_count; i++) {
        free(tokens[i]);
    }

    return result;
}


static int kvs_recovery_fields_protocol(
    const kvs_slice_t *fields,
    size_t field_count,
    char *response,
    int response_capacity)
{
    if (response == NULL || response_capacity <= 0) {
        return -1;
    }

    /* 把 AOF 提供的响应空间包装成命令执行器需要的结构。 */
    kvs_output_buffer_t output = {
        .data = response,
        .capacity = response_capacity
    };

    kvs_exec_status_t execution_status = KVS_EXEC_FAILED;
    int response_length = kvs_execute_fields(
        fields,
        field_count,
        &output,
        KVS_COMMAND_SOURCE_RECOVERY,
        &execution_status
    );

    if (response_length < 0 || execution_status != KVS_EXEC_OK) {
        return -1;
    }
    return response_length;
}

/* 执行 Replica 从 Primary 收到的长度格式增量命令。 */
static int kvs_replication_command_protocol(
    const kvs_slice_t *fields,
    size_t field_count,
    char *response,
    int response_capacity)
{
    if (response == NULL || response_capacity <= 0) {
        return -1;
    }

    /* 将复制模块提供的响应空间包装成统一的输出缓冲区。 */
    kvs_output_buffer_t output = {
        .data = response,
        .capacity = response_capacity
    };

    kvs_exec_status_t execution_status = KVS_EXEC_FAILED;
    int response_length = kvs_execute_fields(
        fields,
        field_count,
        &output,
        KVS_COMMAND_SOURCE_REPLICATION,
        &execution_status
    );

    if (response_length < 0 || execution_status != KVS_EXEC_OK) {
        return -1;
    }
    return response_length;
}

static int kvs_network_protocol(
    int connection_fd,
    char *msg,
    int length,
    kvs_output_buffer_t *response,
    int *consumed_length)
{
    (void)connection_fd;
    if (msg == NULL ||
    response == NULL ||
    response->data == NULL ||
    consumed_length == NULL ||
    length < 0 ||
    response->capacity <= 0) {
    return -1;
}

    *consumed_length = 0;
    if (length == 0) return 0;

        kvs_slice_t fields[KVS_MAX_TOKENS];
        size_t field_count = 0;
        size_t parsed_bytes = 0;

        int status = kvs_parse_command_limited(
            msg, (size_t)length,
            fields, KVS_MAX_TOKENS,
            &field_count, &parsed_bytes,
            KVS_MAX_CLIENT_COMMAND_BYTES
        );

        if (status < 0 ||
        (status == 0 &&
        (size_t)length >= KVS_MAX_CLIENT_COMMAND_BYTES)) {
        static const char error_reply[] =
        "-ERR invalid or oversized request\r\n";
        const size_t error_length = sizeof(error_reply) - 1;

        if (error_length > (size_t)response->capacity) return -1;
        memcpy(response->data, error_reply, error_length);
        *consumed_length = length;
        return (int)error_length;
    }

        if (status == 0) return 0; /* 合法但尚未收完整，继续等。 */

        int response_length = kvs_execute_fields(
        fields,
        field_count,
        response,
        KVS_COMMAND_SOURCE_CLIENT,
        NULL
);
        if (response_length < 0 || response_length > response->capacity) {
            return -1;
        }

        *consumed_length = (int)parsed_bytes;
        return response_length;

}
int init_kvengine(void){

#if ENABLE_ARRAY
    memset(&global_array,0,sizeof(kvs_array_t));
    kvs_array_create(&global_array);
#endif

#if ENABLE_RBTREE
    memset(&global_rbtree,0,sizeof(kvs_rbtree_t));
    kvs_rbtree_create(&global_rbtree);
#endif

#if ENABLE_HASH
    memset(&global_hash,0,sizeof(kvs_hash_t));
    kvs_hash_create(&global_hash);
#endif

#if ENABLE_SKIPLIST
    memset(&global_skiplist,0,sizeof(kvs_skiplist_t));
    kvs_skiplist_create(&global_skiplist);
#endif
    return 0;
}

void dest_kvengine(void){

#if ENABLE_ARRAY
    kvs_array_destory(&global_array);
#endif

#if ENABLE_RBTREE
    kvs_rbtree_destory(&global_rbtree);
#endif

#if ENABLE_HASH
    kvs_hash_destory(&global_hash);
#endif

#if ENABLE_SKIPLIST
    kvs_skiplist_destory(&global_skiplist);
#endif
}

/*
 * 用 Primary 传来的 Snapshot 替换 Replica 当前的内存数据。
 *
 * replication 模块只负责在合适的时间调用这个函数；
 * Engine 的销毁、初始化和恢复仍由 kvstore 模块负责。
 */
static int kvs_install_replication_snapshot(
    const char *snapshot_path)
{
    if(snapshot_path==NULL || snapshot_path[0]=='\0'){
        return -1;
    }

    // 删除 Replica 内存中原来的完整数据集，避免遗留旧 key。
    dest_kvengine();

    // 重新创建一个空的 KV Engine。
    if(init_kvengine()<0){
        return -1;
    }

    /*
     * 逐行读取 Snapshot，并通过 RECOVERY 来源执行 SET/HSET 等命令。
     * 恢复命令不会被当成普通客户端写入。
     */
    long long snapshot_offset=kvs_snapshot_load(
        snapshot_path,
        kvs_recovery_fields_protocol
    );

    if(snapshot_offset<0){
        /*
         * 加载失败时，Snapshot 可能只恢复了一部分。
         * 再次清空，避免对外提供半套数据。
         */
        dest_kvengine();
        init_kvengine();
        return -1;
    }

    if(kvs_aof_reset_to_offset(snapshot_offset)<0){
        /*
         * AOF 无法对齐时，不能继续提供刚加载的数据，
         * 否则当前内存与重启后的恢复结果可能不一致。
         */
        dest_kvengine();
        init_kvengine();
        return -1;
    }



    return 0;
}

int main(int argc, char *argv[])
{
    /* 唯一正式入口：启动参数必须是配置文件路径。 */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <config-file>\n", argv[0]);
        return 1;
    }

    /*
     * 配置模块负责默认值、解析和校验。
     * config 自己持有地址字符串，直到 main() 结束都有效。
     */
    kvs_config_t config = {0};
    char error[256] = {0};

    if (kvs_config_load(argv[1], &config, error, sizeof(error)) < 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

        /* 配置解析成功后，运行日志采用配置文件指定的级别。 */
    kvs_log_set_level(config.log_level);

        /* 保持原有的 Snapshot + AOF 混合恢复流程。 */
    kvs_server_role = config.role;
    init_kvengine();

    long long offset = kvs_snapshot_load(
        "snapshot.db", kvs_recovery_fields_protocol
    );
    if (offset < 0) {
        fprintf(stderr, "failed to load snapshot\n");
        return -1;
    }

    if (kvs_aof_replay(
            "appendonly.aof", offset, kvs_recovery_fields_protocol
        ) < 0) {
        fprintf(stderr, "failed to replay AOF\n");
        return -1;
    }

    if (kvs_aof_open("appendonly.aof") < 0) {
        fprintf(stderr, "failed to open AOF\n");
        return -1;
    }
    /* 以下恢复、复制和网络启动代码继续共用这份配置。 */

if (kvs_replication_init(
        config.role,
        config.primary_host,
        config.primary_port,
        kvs_install_replication_snapshot,
        kvs_replication_command_protocol
    ) < 0) {
            fprintf(stderr, "failed to initialize replication\n");
            kvs_aof_close();
            dest_kvengine();
            return -1;
        }
    int network_ret=-1;

    if (config.role == KVS_ROLE_PRIMARY) {
        kvs_listener_config_t listeners[] = {
            {
                .port = config.service_port,                 // 普通客户端端口
                .bind_ip = config.bind_ip,
                .handler = kvs_network_protocol,             // 客户端命令协议、
                .stream_handler = NULL                       // 普通客户端不发送文件流
            },
            {
                .port = config.replication_port,             // Replica 专用端口
                .bind_ip = config.bind_ip,
                .handler = kvs_replication_network_protocol, // 主从复制协议
                .stream_handler = kvs_replication_stream // 分块提供 Snapshot
            }
        };

         /* Primary 同时开启客户端端口和复制端口。 */
        network_ret = ntyco_start_listeners(
            listeners,
            sizeof(listeners) / sizeof(listeners[0])
        );
    } else if (config.role == KVS_ROLE_REPLICA) {
        /* Replica 监听客户端端口，同时需要连接上游 Primary。 */
        kvs_listener_config_t listener = {
            .port = config.service_port,
            .bind_ip = config.bind_ip,
            .handler = kvs_network_protocol,
            .stream_handler = NULL
        };

        kvs_connector_config_t connector;

        if (kvs_replication_build_connector_config(&connector) < 0) {
            fprintf(stderr, "failed to build replication connector\n");
            network_ret = -1;
        } else {
            network_ret = ntyco_start_runtime(
                &listener, 1, &connector, 1
            );
        }
    } else {
        /* Standalone 只有一个监听器，没有上游连接器。 */
        kvs_listener_config_t listener = {
            .port = config.service_port,
            .bind_ip = config.bind_ip,
            .handler = kvs_network_protocol,
            .stream_handler = NULL
        };
        network_ret = ntyco_start_listeners(&listener, 1);
    }

    if(network_ret<0){
        fprintf(stderr,"failed to start network service\n");
        kvs_replication_destroy();
        kvs_aof_close();
        dest_kvengine();
        return -1;
    }
    kvs_replication_destroy();
    kvs_aof_close();
    dest_kvengine();


}


