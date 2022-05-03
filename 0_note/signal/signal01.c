#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

void handler()
{
    printf("SIG_INT respond\n");
    return;
}

// 编译运行此函数，按一下步骤输入：
// 程序运行到getchar(), 输入ctrl+c，此时并未看见执行信号处理函数
// 输入'q'，退出循环，再按ctrl+c，看见控制端打印SIG_INT respond
int main()
{
    char tmp = 'a';
    sigset_t bset; //用来设置阻塞的信号集

    sigemptyset(&bset); //清空信号集
    sigaddset(&bset, SIGINT); //将SIG_INT信号
    // sigaction 替代它。触发时，SIGINT 信号会复原
    if(signal(SIGINT, handler) == SIG_ERR)
        perror("signal err:");
    sigprocmask(SIG_BLOCK, &bset, NULL); //阻塞SIG_INT信号
    while(tmp != 'q' )
    {
        tmp = getchar();
    }
    sigprocmask(SIG_UNBLOCK, &bset, NULL);//解锁阻塞

    pause();
    return 0;
}