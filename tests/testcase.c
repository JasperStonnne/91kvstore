#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<arpa/inet.h>
#include<sys/time.h>

#define MAX_MSG_LENGTH 1024
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)
int send_msg(int connfd,char *msg,int length){

    int res=send(connfd,msg,length,0);
    if(res<0){
        perror("send");
        exit(1);
    }
    return res;
}
int recv_msg(int connfd,char *msg,int length){

    int res=recv(connfd,msg,length,0);
    if(res<0){
        perror("recv");
        exit(1);



    }
    return res;    
}



void testcase(int connfd,char *msg,char *pattern,char *casename){
    if(!msg||!pattern||!casename) return;
    send_msg(connfd,msg,strlen(msg));

    char result[MAX_MSG_LENGTH]={0};
    recv_msg(connfd,result,MAX_MSG_LENGTH);

    if(strcmp(result,pattern)==0){
        //printf("==>PASS-> %s\n",casename);
    }else{
        printf("==>FAILED-> %s,%s!=%s\n",casename,result,pattern); 
        exit(1);
    }


}


int connect_tcpserver(const char *ip,unsigned short port){
    int connfd=socket(AF_INET,SOCK_STREAM,0);

    struct sockaddr_in server_addr;
    memset(&server_addr,0,sizeof(struct sockaddr_in));

    server_addr.sin_family=AF_INET;
    server_addr.sin_addr.s_addr=inet_addr(ip);
    server_addr.sin_port=htons(port);
    if(0!=connect(connfd,(struct sockaddr*)&server_addr,sizeof(struct sockaddr_in))){

        perror("connect");
        return -1;
    }


    return connfd;
}
//写函数的要义是一个函数只做一件事
void array_testcase(int connfd){
    testcase(connfd,"SET Dad Jasper","OK\r\n","SET-Dad");
    testcase(connfd,"GET Dad","Jasper\r\n","GET-Dad"); 
    testcase(connfd,"MOD Dad Sao","OK\r\n","MOD-Dad");
    testcase(connfd,"GET Dad","Sao\r\n","GET-Dad");
    testcase(connfd,"EXIST Dad","EXIST\r\n","EXIST-Dad");
    testcase(connfd,"DEL Dad","OK\r\n","DEL-Dad"); 
    testcase(connfd,"GET Dad","NO EXIST\r\n","GET-Dad");
    testcase(connfd,"MOD Dad Jasper","NO EXIST\r\n","MOD-Dad");
    testcase(connfd,"EXIST Dad","NO EXIST\r\n","EXIST-Dad");      


}

void array_testcase_10w(int connfd){
    int count =10000;
    int i=0;
    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);
    for(i=0;i<count;i++){
    testcase(connfd,"SET Dad Jasper","OK\r\n","SET-Dad");
    testcase(connfd,"GET Dad","Jasper\r\n","GET-Dad"); 
    testcase(connfd,"MOD Dad Sao","OK\r\n","MOD-Dad");
    testcase(connfd,"GET Dad","Sao\r\n","GET-Dad");
    testcase(connfd,"EXIST Dad","EXIST\r\n","EXIST-Dad");
    testcase(connfd,"DEL Dad","OK\r\n","DEL-Dad"); 
    testcase(connfd,"GET Dad","NO EXIST\r\n","GET-Dad");
    testcase(connfd,"MOD Dad Jasper","NO EXIST\r\n","MOD-Dad");
    testcase(connfd,"EXIST Dad","NO EXIST\r\n","EXIST-Dad");      
}
    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);
    int time_used=TIME_SUB_MS(tv_end,tv_begin);//ms
    printf("arrary testcase --->time_used: %d,qps: %d",time_used,90000*1000/time_used);
}

void rbtree_testcase(int connfd){

    testcase(connfd,"RSET Dad Jasper","OK\r\n","RSET-Dad");
    testcase(connfd,"RGET Dad","Jasper\r\n","RGET-Dad"); 
    testcase(connfd,"RMOD Dad Sao","OK\r\n","RMOD-Dad");
    testcase(connfd,"RGET Dad","Sao\r\n","RGET-Dad");
    testcase(connfd,"REXIST Dad","EXIST\r\n","REXIST-Dad");
    testcase(connfd,"RDEL Dad","OK\r\n","RDEL-Dad"); 
    testcase(connfd,"RGET Dad","NO EXIST\r\n","RGET-Dad");
    testcase(connfd,"RMOD Dad Jasper","NO EXIST\r\n","RMOD-Dad");
    testcase(connfd,"REXIST Dad","NO EXIST\r\n","REXIST-Dad");      


}

void rbtree_testcase_10w(int connfd){
    int count =10000;
    int i=0;
    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);
    for(i=0;i<count;i++){
    testcase(connfd,"RSET Dad Jasper","OK\r\n","RSET-Dad");
    testcase(connfd,"RGET Dad","Jasper\r\n","RGET-Dad"); 
    testcase(connfd,"RMOD Dad Sao","OK\r\n","RMOD-Dad");
    testcase(connfd,"RGET Dad","Sao\r\n","RGET-Dad");
    testcase(connfd,"REXIST Dad","EXIST\r\n","REXIST-Dad");
    testcase(connfd,"RDEL Dad","OK\r\n","RDEL-Dad"); 
    testcase(connfd,"RGET Dad","NO EXIST\r\n","RGET-Dad");
    testcase(connfd,"RMOD Dad Jasper","NO EXIST\r\n","RMOD-Dad");
    testcase(connfd,"REXIST Dad","NO EXIST\r\n","REXIST-Dad");      

}
    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);
    int time_used=TIME_SUB_MS(tv_end,tv_begin);//ms
    printf("rbtree testcase --->time_used: %d,qps: %d",time_used,90000*1000/time_used);
}

void rbtree_testcase_3w(int connfd){
    int count =10000;
    int i=0;
    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    for(i=0;i<count;i++){
        char cmd[128]={0};
        snprintf(cmd,128,"RSET Dad%d Jasper%d",i,i);
        testcase(connfd,cmd,"OK\r\n","RSET-Dad");
    }
    for(i=0;i<count;i++){
        char cmd[128]={0};
        snprintf(cmd,128,"RGET Dad%d",i);
        char result[128]={0};
        snprintf(result,128,"Jasper%d\r\n",i);

        testcase(connfd,cmd,result,"RGET-Jasper-Dad");
    }
    for(i=0;i<count;i++){
        char cmd[128]={0};
        snprintf(cmd,128,"RMOD Dad%d Jasper%d",i,i);
        testcase(connfd,cmd,"OK\r\n","RGET-Jasper-Dad");
    }


    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);
    int time_used=TIME_SUB_MS(tv_end,tv_begin);//ms
    printf("rbtree testcase --->time_used: %d,qps: %d",time_used,30000*1000/time_used); 

}
void hash_testcase_10w(int connfd){
    int count =10000;
    int i=0;
    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);
    for(i=0;i<count;i++){
    testcase(connfd, "HSET Dad Jasper", "OK\r\n", "HSET-Dad");
    testcase(connfd, "HDEL Dad", "OK\r\n", "HDEL-Dad");

}
    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);
    int time_used=TIME_SUB_MS(tv_end,tv_begin);//ms
    int total_requests = count * 2;
    int qps = total_requests * 1000 / time_used;

    printf("hash allocator benchmark ---> "
        "requests: %d, time_used: %d ms, qps: %d\n",
        total_requests,
        time_used,
        qps);
    }
void skiplist_testcase_10w(int connfd){
    int count =10000;
    int i=0;
    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);
    for(i=0;i<count;i++){
    testcase(connfd,"SSET Dad Jasper","OK\r\n","HSET-Dad");
    testcase(connfd,"SGET Dad","Jasper\r\n","HGET-Dad"); 
    testcase(connfd,"SMOD Dad Sao","OK\r\n","HMOD-Dad");
    testcase(connfd,"SGET Dad","Sao\r\n","HGET-Dad");
    testcase(connfd,"SEXIST Dad","EXIST\r\n","HEXIST-Dad");
    testcase(connfd,"SDEL Dad","OK\r\n","HDEL-Dad"); 
    testcase(connfd,"SGET Dad","NO EXIST\r\n","HGET-Dad");
    testcase(connfd,"SMOD Dad Jasper","NO EXIST\r\n","HMOD-Dad");
    testcase(connfd,"SEXIST Dad","NO EXIST\r\n","HEXIST-Dad");      

}
    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);
    int time_used=TIME_SUB_MS(tv_end,tv_begin);//ms
    printf("skiplist testcase --->time_used: %d,qps: %d",time_used,90000*1000/time_used);
}
//testcase 172.16.145.129 2000
int main(int argc,char *argv[]){

    if(argc!=4){

        printf("arg error\n");
        return -1;
    }
    char *ip=argv[1];
    int port=atoi(argv[2]);
    int mode=atoi(argv[3]);
    int connfd=connect_tcpserver(ip,port);

    if(mode==0){
        rbtree_testcase_10w(connfd);
    }else if(mode==1){
        rbtree_testcase_3w(connfd);
    }else if(mode==2){
        array_testcase_10w(connfd); 
    }else if(mode==3){
        hash_testcase_10w(connfd);
    }else if(mode==4){
        skiplist_testcase_10w(connfd);
    }
    
    
    return 0;
}
