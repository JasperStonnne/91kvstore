//kvstore.c

#include"kvstore.h"
#include "persistence.h"

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
static int aof_replaying = 0;
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
//SET Key Value
//tokens[0]:SET
//tokens[1]:Key
//tokens[2]:Value
int kvs_filter_protocol(char **tokens,int count,char *response){

    if(tokens[0]==NULL||count==0||response==NULL)return -1;

    int cmd= KVS_CMD_START;
    for(cmd=KVS_CMD_START;cmd<KVS_CMD_COUNT;cmd++){
        if(strcmp(tokens[0],command[cmd])==0){
            break;
        }
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
    char *result=kvs_array_get(&global_array,key);
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
    char *result=kvs_rbtree_get(&global_rbtree,key); 
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
    char *result=kvs_hash_get(&global_hash,key); 
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
    char *result=kvs_skiplist_get(&global_skiplist,key); 
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
    if(!aof_replaying&&ret==0&&kvs_is_write_command(cmd)){
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




/*
msg:request message
length: length of request message
response:need to send
@return: length of response

*/

int kvs_protocol(char *msg,int length,char *response){

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
    if(count==1&&strcmp(tokens[0],"SAVE")==0){
        int save_result=kvs_snapshot_save("snapshot.db");
        if(save_result<0){
            return sprintf(response,"ERROR\r\n");
        }
        return sprintf(response, "OK\r\n");
    }
    //memcpy(response,msg,length);
    return kvs_filter_protocol(tokens,count,response);
}

static int kvs_find_crlf(const char *msg,int length){
    if(msg==NULL||length<2){
        return -1;
    }
    for(int i=0;i+1<length;i++){
        if(msg[i]=='\r'&&msg[i+1]=='\n'){
            return i;
        }
    }
    return -1;
}

int kvs_batch_protocol(char *msg,int length,char *response,int response_capacity){
    if(msg==NULL||length<=0||response==NULL||response_capacity<=0){
        return -1;
    }
    int request_offset=0;//处理到请求的什么位置
    int response_offset=0;//当前写入了多少响应
    while(request_offset<length){
        char *command_start=msg+request_offset;
        int remaining_length=length-request_offset;
        int command_end=kvs_find_crlf(command_start,remaining_length);
        if(command_end<0){
            break;
        }
        command_start[command_end]='\0';
        if(command_end==0){
            return -1;
        }
        if (response_offset >= response_capacity) {
            return -1;
        }
        int response_length=kvs_protocol(command_start,command_end,response+response_offset);
        if (response_length < 0 ||response_length > response_capacity - response_offset) {
            return -1;
        }
        response_offset+=response_length;
        request_offset+=command_end+2;
    }
    return response_offset;
    
}

static int kvs_network_protocol(char *msg,int length,char *response){
    return kvs_batch_protocol(msg,length,response,1024);
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



int main(int argc,char *argv[]){
    if(argc!=2) return -1;

    int port =atoi(argv[1]);
    init_kvengine();
    aof_replaying =1;
    long long offset=kvs_snapshot_load("snapshot.db",kvs_protocol);
    if(offset<0){
        aof_replaying=-1;
        fprintf(stderr,"failed to load snapshot\n");
        return -1;
    }
    int replay_ret=kvs_aof_replay("appendonly.aof",offset,kvs_protocol);
    aof_replaying=0;
    if(replay_ret<0){
        fprintf(stderr, "failed to replay AOF\n");
        return -1;
    }

    if (kvs_aof_open("appendonly.aof") < 0) {
        fprintf(stderr, "failed to open AOF\n");
        return -1;
}
#if (NETWORK_SELECT==NETWORK_REACTOR)
    reactor_start(port,kvs_network_protocol);
#elif(NETWORK_SELECT==NETWORK_NTYCO)
    ntyco_start(port,kvs_network_protocol);
#elif(NETWORK_SELECT==NETWORK_PROACTOR)
    proactor_start(port,kvs_network_protocol);
#endif

    dest_kvengine();


}


