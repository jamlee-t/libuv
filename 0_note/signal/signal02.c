#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

void printsigset(sigset_t *set) 
{
    int i;
    for (i=1; i<NSIG; ++i)
    {
        if (sigismember(set, i))//置1的位，说明对应信号在信号集 set中。返回真，则打印1。如01000000000000....说明2号信号是处于未决状态
            putchar('1');
        else
            putchar('0');
    }
    printf("\n");
}

int main()
{
    char tmp = 'a';
    sigset_t bset; //用来设置阻塞的信号集
    sigset_t pset; //用来打印未决信号集
    sigemptyset(&bset); //清空信号集
    sigaddset(&bset, SIGINT); //将SIG_INT信号添加到信号集中
    sigprocmask(SIG_BLOCK, &bset, NULL); //阻塞SIG_INT信号
    for(;;)
    {
        //获取未决字信息
        sigpending(&pset); //若一信号是未决状态，则将set对应位置1
        //打印信号未决 sigset_t字
        printsigset(&pset);
        sleep(1);
    }
    pause();
    return 0;
}