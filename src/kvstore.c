//kvstore.c

#include"kvstore.h"
#include "persistence.h"
#include "replication.h"
#include "server.h"
#include "protocol.h"
#include<errno.h>
#include<limits.h>
#include <stdint.h>
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


int kvs_split_token(char *msg,char*tokens[]){
    //每一个函数 进入时需要做参数判断避免出错
    if(msg==NULL||tokens==NULL) return -1;
    int idx=0;
    char *token=strtok(msg," ");
    while(token!=NULL){
        //printf("idx:%d,%s\n",idx,token);

        tokens[idx++]=token;
        token=strtok(NULL," ");


    }
    return idx;




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
int kvs_filter_protocol(char **tokens,int count,char *response,kvs_command_source_t source){

    if(tokens[0]==NULL||count==0||response==NULL)return -1;

    int cmd= KVS_CMD_START;
    for(cmd=KVS_CMD_START;cmd<KVS_CMD_COUNT;cmd++){
        if(strcmp(tokens[0],command[cmd])==0){
            break;
        }
    }

    if (cmd == KVS_CMD_COUNT) {
    return sprintf(response, "ERROR unknown command\r\n");
    }

    if(kvs_server_role==KVS_ROLE_REPLICA&&source==KVS_COMMAND_SOURCE_CLIENT&&kvs_is_write_command(cmd)){
        return sprintf(response,"READONLY replica does not accept client writes\r\n");
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
            length=sprintf(response,"ERROR\r\n");
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");  
        }else{
            length=sprintf(response,"EXIST\r\n");
        }
        
        break;
    case KVS_CMD_GET:{
    char *result = kvs_get_value(cmd, key);
    if(result==NULL){
        length=sprintf(response,"NO EXIST\r\n");
    }else {
        length=sprintf(response,"%s\r\n",result); 
    }
        break;
}
    case KVS_CMD_DEL:
        ret=kvs_array_del(&global_array,key);
        if(ret<0){
           length=sprintf(response,"ERROR\r\n"); 
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");  
        }
        break;
    case KVS_CMD_MOD:
        ret=kvs_array_mod(&global_array,key,value);
        if(ret<0){
           length=sprintf(response,"ERROR\r\n"); 
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");  
        }
        break;
    case KVS_CMD_EXIST:
        ret=kvs_array_exist(&global_array,key);
        if(ret==0){
        length=sprintf(response,"EXIST\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");              
        }
        break;
#endif
#if ENABLE_RBTREE
//rbtree
    case KVS_CMD_RSET:
        ret=kvs_rbtree_set(&global_rbtree,key,value);
        if(ret<0){
            length=sprintf(response,"ERROR\r\n");
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");  
        }else{
            length=sprintf(response,"EXIST\r\n");
        }
        
        break;
    case KVS_CMD_RGET:{
    char *result = kvs_get_value(cmd, key);
    if(result==NULL){
        length=sprintf(response,"NO EXIST\r\n");
    }else {
        length=sprintf(response,"%s\r\n",result); 
    }
        break;
}
    case KVS_CMD_RDEL:
        ret=kvs_rbtree_del(&global_rbtree,key);
        if(ret<0){
           length=sprintf(response,"ERROR\r\n"); 
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");  
        }
        break;
    case KVS_CMD_RMOD:
        ret=kvs_rbtree_mod(&global_rbtree,key,value);
        if(ret<0){
           length=sprintf(response,"ERROR\r\n"); 
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");  
        }
        break;
    case KVS_CMD_REXIST:
        ret=kvs_rbtree_exist(&global_rbtree,key);
        if(ret==0){
        length=sprintf(response,"EXIST\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");              
        }
        break;        
#endif
#if ENABLE_HASH
    case KVS_CMD_HSET:
        ret=kvs_hash_set(&global_hash,key,value);
        if(ret<0){
            length=sprintf(response,"ERROR\r\n");
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");  
        }else{
            length=sprintf(response,"EXIST\r\n");
        }
        
        break;
    case KVS_CMD_HGET:{
    char *result = kvs_get_value(cmd, key);
    if(result==NULL){
        length=sprintf(response,"NO EXIST\r\n");
    }else {
        length=sprintf(response,"%s\r\n",result); 
    }
        break;
}
    case KVS_CMD_HDEL:
        ret=kvs_hash_del(&global_hash,key);
        if(ret<0){
           length=sprintf(response,"ERROR\r\n"); 
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");  
        }
        break;
    case KVS_CMD_HMOD:
        ret=kvs_hash_mod(&global_hash,key,value);
        if(ret<0){
           length=sprintf(response,"ERROR\r\n"); 
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");  
        }
        break;
    case KVS_CMD_HEXIST:
        ret=kvs_hash_exist(&global_hash,key);
        if(ret==0){
        length=sprintf(response,"EXIST\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");              
        }
        break;        
#endif
#if ENABLE_SKIPLIST
    case KVS_CMD_SSET:
        ret=kvs_skiplist_set(&global_skiplist,key,value);
        if(ret<0){
            length=sprintf(response,"ERROR\r\n");
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");  
        }else{
            length=sprintf(response,"EXIST\r\n");
        }
        
        break;
    case KVS_CMD_SGET:{
    char *result = kvs_get_value(cmd, key);
    if(result==NULL){
        length=sprintf(response,"NO EXIST\r\n");
    }else {
        length=sprintf(response,"%s\r\n",result); 
    }
        break;
}
    case KVS_CMD_SDEL:
        ret=kvs_skiplist_del(&global_skiplist,key);
        if(ret<0){
           length=sprintf(response,"ERROR\r\n"); 
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");  
        }
        break;
    case KVS_CMD_SMOD:
        ret=kvs_skiplist_mod(&global_skiplist,key,value);
        if(ret<0){
           length=sprintf(response,"ERROR\r\n"); 
        }else if(ret==0){
            length=sprintf(response,"OK\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");  
        }
        break;
    case KVS_CMD_SEXIST:
        ret=kvs_skiplist_exist(&global_skiplist,key);
        if(ret==0){
        length=sprintf(response,"EXIST\r\n");
        }else{
            length=sprintf(response,"NO EXIST\r\n");              
        }
        break;        

#endif

    default:
        assert(0);
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

static int kvs_trim_command_end(char *msg,int length){
    if(msg==NULL||length<=0){
        return -1;
    }
    while(length>0&&(msg[length-1]=='\r'||msg[length-1]=='\n')){
        msg[length-1]='\0';
        length--;
    }
    return length;
}

/* 统一执行已经拆好的命令字段，供旧格式和新格式共用。 */
static int kvs_execute_tokens(
    char **tokens,
    int count,
    char *response,
    kvs_command_source_t source)
{
    if (tokens == NULL || count <= 0 ||
        tokens[0] == NULL || response == NULL) {
        return -1;
    }

    if (count == 1 && strcmp(tokens[0], "SAVE") == 0) {
        int result = kvs_snapshot_save("snapshot.db", NULL);
        return sprintf(response, result < 0 ? "ERROR\r\n" : "OK\r\n");
    }

    return kvs_filter_protocol(tokens, count, response, source);
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
    kvs_command_source_t source)
{
    char *tokens[KVS_MAX_TOKENS] = {0};

    if (response == NULL ||
        response->data == NULL ||
        response->capacity <= 0 ||
        field_count > INT_MAX ||
        kvs_fields_to_tokens(
            fields, field_count, tokens, KVS_MAX_TOKENS) != 0) {
        return -1;
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
                result = sprintf(response->data, "NO EXIST\r\n");
            } else {
                result = kvs_encode_value_to_output(response, value);
            }

            for (size_t i = 0; i < field_count; i++) {
                free(tokens[i]);
            }

            return result;
        }
    }

    int result = kvs_execute_tokens(
        tokens,
        (int)field_count,
        response->data,
        source
    );

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

    return kvs_execute_fields(
        fields,
        field_count,
        &output,
        KVS_COMMAND_SOURCE_RECOVERY
    );
}


/*
msg:request message
length: length of request message
response:need to send
@return: length of response

*/

static int kvs_execute_command(char *msg,int length,char *response,kvs_command_source_t source){

//SET Key Value
//GET Key
//DEL Key
    if (msg==NULL||length<=0||response==NULL) return -1;
    length = kvs_trim_command_end(msg, length);
    if(length<=0){
        return -1;
    }
    printf("recv: %d: %s\n",length,msg);

    char *tokens[KVS_MAX_TOKENS]={0};

    int count=kvs_split_token(msg,tokens);//count的作用是 有多少个tokens
    if (count==-1) return -1;
    return kvs_execute_tokens(tokens, count, response, source);
}
//适配三参数客户端接口 并且补充CLIENT 命令来源（适配起函数
static int kvs_client_protocol(int connection_fd,char *msg,int length,char *response,int response_capacity){
    (void)connection_fd;
    (void)response_capacity;
    return kvs_execute_command(msg,length,response,KVS_COMMAND_SOURCE_CLIENT);
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

    return kvs_execute_fields(
        fields,
        field_count,
        &output,
        KVS_COMMAND_SOURCE_REPLICATION
    );
}

int kvs_batch_protocol(
    char *msg,
    int length,
    char *response,
    int response_capacity,
    int *consumed_length)
{
    return kvs_line_batch_protocol(
        -1,                         // 兼容旧接口，这里没有传入连接 fd
        msg,                        // TCP 输入缓冲区
        length,                     // 当前已有数据长度
        response,                   // 响应缓冲区
        response_capacity,          // 响应缓冲区容量
        consumed_length,            // 返回已经完整处理的字节数
        kvs_client_protocol         // 每拆出一条命令，就交给它执行
    );
}

static int kvs_network_protocol(
    int connection_fd,
    char *msg,
    int length,
    kvs_output_buffer_t *response,
    int *consumed_length)
{
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

    if (msg[0] == '*') {
        kvs_slice_t fields[KVS_MAX_TOKENS];
        size_t field_count = 0;
        size_t parsed_bytes = 0;

        int status = kvs_parse_command(
            msg, (size_t)length,
            fields, KVS_MAX_TOKENS,
            &field_count, &parsed_bytes
        );

        if (status <= 0) return status; /* 0：等更多数据；-1：格式错误 */

        int response_length = kvs_execute_fields(
    fields,
    field_count,
    response,
    KVS_COMMAND_SOURCE_CLIENT
);
        if (response_length < 0 || response_length > response->capacity) {
            return -1;
        }

        *consumed_length = (int)parsed_bytes;
        return response_length;
    }

    /* 旧格式暂时仍按行处理。 */
    return kvs_line_batch_protocol(
        connection_fd, msg, length, response->data,
        response->capacity, consumed_length,
        kvs_client_protocol
    );
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

static int kvs_parse_port(const char *text,unsigned short *port){
    if(text==NULL||port==NULL){
        return -1;
    }
    errno=0;
    char *end =NULL;//字符串解析结束的位置
    long value =strtol(text,&end,10);//按十进制 把字符转化为long
    if(errno!=0||end==text||*end!='\0'||value<1||value>65535){
        return -1;
    }
    *port=(unsigned short)value;
    return 0;
}

int main(int argc,char *argv[]){
    kvs_server_config_t config={
        .role=KVS_ROLE_STANDALONE,
        .service_port=0,
        .replication_port=0,
        .primary_host =NULL
    };
    if(argc==2){

        if(kvs_parse_port(argv[1],&config.service_port)<0){
            fprintf(stderr,"invalid service port: %s\n",argv[1]);
            return -1;
        }
    }else if(argc==4 &&strcmp(argv[1],"primary")==0){
        config.role=KVS_ROLE_PRIMARY;

        if(kvs_parse_port(argv[2],&config.service_port)<0){
            fprintf(stderr,"invalid service port:%s\n",argv[2]);
            return -1;
        }
        if(kvs_parse_port(argv[3],&config.replication_port)<0){
            fprintf(stderr,"invalid replication port:%s\n",argv[3]);
            return -1;
        }
        if(config.service_port==config.replication_port){
            fprintf(stderr,"service port and replication port must differ\n");
            return -1;
        }

    }else if(argc==5&&strcmp(argv[1],"replica")==0){
        config.role=KVS_ROLE_REPLICA;

        if(kvs_parse_port(argv[2],&config.service_port)<0){
            fprintf(stderr,"invalid service port:%s\n",argv[2]);
            return -1;
        }
        if(argv[3][0]=='\0'){
            fprintf(stderr,"primary host cannot be empty\n");
            return -1;
        }
        config.primary_host =argv[3];
        if(kvs_parse_port(argv[4],&config.replication_port)<0){
            fprintf(stderr,"invalid replication port:%s\n",argv[4]);
            return -1;
        }

    }else{
        fprintf(stderr,
                "usage:\n"
                "  %s <service-port>\n"
                "  %s primary <service-port> <replication-port>\n"
                "  %s replica <service-port> <primary-host> <replication-port>\n",
                argv[0],
                argv[0],
                argv[0]);
        return -1;

    }

    kvs_server_role = config.role;
    init_kvengine();
    long long offset=kvs_snapshot_load("snapshot.db", kvs_recovery_fields_protocol);
    if(offset<0){
        fprintf(stderr,"failed to load snapshot\n");
        return -1;
    }
    int replay_ret=kvs_aof_replay("appendonly.aof",offset,kvs_recovery_fields_protocol);
    if(replay_ret<0){
        fprintf(stderr, "failed to replay AOF\n");
        return -1;
    }

    if (kvs_aof_open("appendonly.aof") < 0) {
        fprintf(stderr, "failed to open AOF\n");
        return -1;
}
    if (kvs_replication_init(&config,kvs_install_replication_snapshot,kvs_replication_command_protocol) < 0) {
    fprintf(stderr, "failed to initialize replication\n");
    kvs_aof_close();
    dest_kvengine();
    return -1;
}
int network_ret=-1;

#if (NETWORK_SELECT == NETWORK_REACTOR)
    if(config.role==KVS_ROLE_PRIMARY){
        kvs_listener_config_t listeners[] = {
            {
                .port=config.service_port,
                .handler=kvs_network_protocol,
                .stream_handler=NULL
            },
            {
                .port=config.replication_port,
                .handler=kvs_replication_network_protocol,
                .stream_handler=kvs_replication_stream
            }
        };

        network_ret=reactor_start_listeners(
            listeners,
            sizeof(listeners)/sizeof(listeners[0])
        );
        }else if(config.role==KVS_ROLE_REPLICA){
        /*
         * Replica 对客户端监听自己的 service_port。
         * 客户端只能通过这里读取数据。
         */
        kvs_listener_config_t listener={
            .port=config.service_port,
            .handler=kvs_network_protocol,
            .stream_handler=NULL
        };

        /*
         * replication.c 负责提供 Primary 地址以及
         * 连接成功、收到消息、连接断开时的业务回调。
         */
        kvs_connector_config_t connector;

        if(kvs_replication_build_connector_config(
                &connector)<0){

            fprintf(
                stderr,
                "failed to build replication connector\n"
            );
            network_ret=-1;
        }else{
            network_ret=reactor_start_runtime(
                &listener,
                1,
                &connector,
                1
            );
        }
    }else{
        /*
         * standalone 没有上游 Primary，只监听客户端端口。
         */
        kvs_listener_config_t listener={
            .port=config.service_port,
            .handler=kvs_network_protocol,
            .stream_handler=NULL
        };

        network_ret=reactor_start_listeners(
            &listener,
            1
        );
    }
#elif (NETWORK_SELECT == NETWORK_NTYCO)
    if (config.role == KVS_ROLE_PRIMARY) {
        kvs_listener_config_t listeners[] = {
            {
                .port = config.service_port,                 // 普通客户端端口
                .handler = kvs_network_protocol,             // 客户端命令协议、
                .stream_handler = NULL                       // 普通客户端不发送文件流
            },
            {
                .port = config.replication_port,             // Replica 专用端口
                .handler = kvs_replication_network_protocol, // 主从复制协议
                .stream_handler = kvs_replication_stream // 分块提供 Snapshot
            }
        };

        network_ret = ntyco_start_listeners(
            listeners,
            sizeof(listeners) / sizeof(listeners[0])          // 两个监听器配置
        );
    } else if(config.role==KVS_ROLE_REPLICA) {
        kvs_listener_config_t listener={
            .port=config.service_port,
            .handler=kvs_network_protocol
        };

        kvs_connector_config_t connector;

        if(kvs_replication_build_connector_config(&connector)<0){
            fprintf(stderr,"failed to build replication connector\n");
            network_ret=-1;
        }else{
            network_ret=ntyco_start_runtime(&listener,1,&connector,1);

        }


    }else{
        network_ret=ntyco_start(config.service_port,kvs_network_protocol);
    }

#elif (NETWORK_SELECT == NETWORK_PROACTOR)
    if(config.role==KVS_ROLE_PRIMARY){
        kvs_listener_config_t listeners[]={
            {
                .port=config.service_port,
                .handler=kvs_network_protocol,
                .stream_handler=NULL
            },
            {
                .port=config.replication_port,
                .handler=kvs_replication_network_protocol,
                .stream_handler=kvs_replication_stream
            }
        };

        network_ret=proactor_start_listeners(
            listeners,
            sizeof(listeners)/sizeof(listeners[0])
        );
    }else if(config.role==KVS_ROLE_REPLICA){
    /*
     * Replica 监听自己的客户端服务端口。
     */
    kvs_listener_config_t listener={
        .port=config.service_port,
        .handler=kvs_network_protocol,
        .stream_handler=NULL
    };

    /*
     * replication.c 提供 Primary 地址和复制回调。
     */
    kvs_connector_config_t connector;

    if(kvs_replication_build_connector_config(
            &connector)<0){

        fprintf(
            stderr,
            "failed to build replication connector\n"
        );
        network_ret=-1;
    }else{
        network_ret=proactor_start_runtime(
            &listener,
            1,
            &connector,
            1
        );
    }
}else{
    /*
     * standalone 只监听客户端服务端口。
     */
    kvs_listener_config_t listener={
        .port=config.service_port,
        .handler=kvs_network_protocol,
        .stream_handler=NULL
    };

    network_ret=proactor_start_listeners(
        &listener,
        1
    );
}
#endif

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


