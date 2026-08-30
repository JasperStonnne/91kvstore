#include<stdio.h>
#include <unistd.h>
#include<errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include<string.h>
#include"persistence.h"

static FILE *aof_fp=NULL;
/*
return: 0-success,-1-error
*/
int kvs_aof_open(const char* path){
    if(path==NULL){
        return -1;
    }
    if(aof_fp!=NULL){
        return -1;
    }
    aof_fp=fopen(path,"a");
    if(aof_fp==NULL){
        return -1;
    }
    return 0;
}
int kvs_aof_close(void){
    if(aof_fp==NULL){
        return -1;
    }
    int ret=fclose(aof_fp);
    aof_fp=NULL;
    if(ret==0){
        return 0;
    }else{
        return -1;
    }
}

int kvs_aof_append(char **tokens, int count){
    if(aof_fp==NULL){
        return -1;
    }
    if(tokens==NULL||(count!=2&&count!=3)){
        return -1;
    }
    if(tokens[0]==NULL||tokens[1]==NULL){
        return -1;
    }
    if(count==3&&tokens[2]==NULL){
        return -1;
    }
    int written;
    if(count==2){
        written = fprintf(aof_fp,"%s %s\n",tokens[0],tokens[1]);
    }else if(count ==3){
        written=fprintf(aof_fp,"%s %s %s\n",tokens[0],tokens[1],tokens[2]);
    }
    if (written < 0) {
        return -1;
    }
    if (fflush(aof_fp) != 0) {
        return -1;
    }
    if (fsync(fileno(aof_fp)) != 0) {
        return -1;
    }
    return 0;
}

int kvs_aof_replay(const char*path,long long offset,aof_replay_handler handler){
    if(path==NULL||handler==NULL||offset<0){
        return -1;
    }
    FILE *fp=fopen(path,"r");
    if(fp==NULL){
        if(errno==ENOENT){
            return 0;
        }
        return -1;
    }
    if(fseeko(fp,(off_t)offset,SEEK_SET)!=0){//fp 文件 ，移动字节数 SEEK_SET从文件开头计算位置
        fclose(fp);
        return -1;
    }
    char *line=NULL;//保存我所读取到的一行
    size_t capacity=0;//当前分配的内存容量
    ssize_t line_length;//实际读取到的字符数
    int replay_result=0;//每次重放的结果
    while((line_length=getline(&line,&capacity,fp))!=-1){
        while (line_length > 0 &&(line[line_length - 1] == '\n' ||line[line_length - 1] == '\r')) {
            line[--line_length] = '\0';
    }
    if(line_length==0){
        continue;
    }
    char response[1024]={0};
    int response_length=handler(line,(int)line_length,response);
    if(response_length<=0||strcmp(response,"OK\r\n")!=0){
        replay_result=-1;
        break;
    }
}
    if(ferror(fp))  {
        replay_result=-1;
    }
    free(line);
    if (fclose(fp) != 0) {
        replay_result = -1;
    }
    return replay_result;
}
long long kvs_aof_get_offset(void){
    if(aof_fp==NULL){
        return -1;
    }
    off_t offset =ftello(aof_fp);
    if(offset==(off_t)-1){
        return -1;
    }
    return (long long)offset;
}