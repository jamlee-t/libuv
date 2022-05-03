// https://blog.csdn.net/zhizhengguan/article/details/111212174

////////////////////////////////////////////////////////////////////
// 父子进程通过 eventfd 通信
////////////////////////////////////////////////////////////////////
#include <malloc.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wait.h>
#include <sys/eventfd.h>

#define handle_error(msg)   \
    do { perror(msg); exit(EXIT_FAILURE);  } while(0)
int main(int argc, char** argv)
{
    int  efd, i;
    uint64_t buf;
    ssize_t rc;

    if(argc < 2){
        fprintf(stderr, "Usage: %s <num>...\n",argv[0]);
        exit(EXIT_FAILURE);
    }

    efd = eventfd(0, 0);
    if(-1 == efd){
        handle_error("eventfd");
    }

    switch(fork()){
        case 0:
            for(i = 1;  i < argc; i++){
                printf("Child writing %s to efd\n",argv[i]);
                buf = atoll(argv[i]);
                rc = write(efd, &buf, sizeof(uint64_t));
                if(rc != sizeof(uint64_t)){
                    handle_error("write");
                }
            }
            printf("Child completed write loop\n");
            exit(EXIT_SUCCESS);
        default:
            sleep(2);
            printf("Parent about to read\n");
            rc = read(efd, &buf, sizeof(uint64_t));
            if(rc != sizeof(uint64_t))
                handle_error("read");
            printf("Parent read %llu from efd\n",(unsigned long long)buf);
        case -1:
            handle_error("fork");
    }
    return 0;
}
