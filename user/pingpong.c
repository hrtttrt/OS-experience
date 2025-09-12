#include "kernel/types.h"
#include "user.h"

int main(int argc,char* argv[]){
    int f2c[2];
    int c2f[2];
    int cpid,fpid,status;
    char num[20];
    char buffer[20];
    pipe(f2c);
    pipe(c2f);
    if(fork()==0)
    {
        //子进程
        cpid=getpid();
        itoa(cpid,num);
        write(c2f[1],num,strlen(num));
        close(c2f[1]);
        read(f2c[0],buffer,sizeof(buffer)-1);
        close(f2c[0]);
        fpid=atoi(buffer);
        printf("%d: received ping from pid %d\n",cpid,fpid);
    }
    else
    {
        //父进程
        fpid=getpid();
        itoa(fpid,num);
        write(f2c[1],num,strlen(num));
        close(f2c[1]);
        read(c2f[0],buffer,sizeof(buffer)-1);
        close(c2f[0]);
        cpid=atoi(buffer);
        if(wait(&status)) printf("%d: received pong from pid %d\n",fpid,cpid);
    }
    exit(0);
}