#include<stdio.h>
#include <unistd.h>
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