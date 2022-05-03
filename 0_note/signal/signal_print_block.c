#include <stdio.h>
#include <signal.h>

void printsigset(sigset_t *set) 
{
    int i;
    for (i=1; i<NSIG; ++i)
    {
        if (sigismember(set, i))//置1的位，说明对应信号在信号集 set中。返回真，则打印1。如01000000000000....说明2号信号是阻塞的
            putchar('1');
        else
            putchar('0');
    }
    printf("\n");
}

int main()
{
    sigset_t bset; // 用来设置阻塞的信号集
    sigset_t oset; // 用来打印阻塞信号集
    sigemptyset(&bset); // 清空信号集
    sigemptyset(&oset); // 清空信号集

    // 输出线程默认的阻塞信号集，是不阻塞任何信号
    // 0000000000000000000000000000000000000000000000000000000000000000
    sigprocmask(SIG_BLOCK, NULL, &oset); 
    printsigset(&oset);

    sigemptyset(&oset); // 清空信号集
    sigaddset(&bset, SIGUSR1); // 将SIGUSR1信号添加到信号集中
    sigaddset(&bset, SIGUSR2); // 将SIGUSR2信号添加到信号集中
    sigprocmask(SIG_BLOCK, &bset, NULL); // 如置阻塞之前除了 SIGUSR1 和 SIGUSR2没有设置阻塞，存在其他信号已设置阻塞。那么会添加 SIGUSR1 和 SIGUSR2 位设置阻塞。
    sigprocmask(SIG_BLOCK, NULL, &oset); // 输出设置结果： 0000000001010000000000000000000000000000000000000000000000000000
    printsigset(&oset);

    // 目前前的阻塞标记为，现在把这个整个重置
    // 0000000001010000000000000000000000000000000000000000000000000000
    sigset_t new_set_overwrite; // 整个完全重新设置阻塞信号集，直接重置为 new_set_overwrite 的值
    sigemptyset(&new_set_overwrite);
    sigaddset(&new_set_overwrite, SIGUSR1); // 将SIGUSR1信号添加到信号集中
    sigprocmask(SIG_SETMASK, &new_set_overwrite, &oset);
    sigprocmask(SIG_BLOCK, NULL, &oset);
    printsigset(&oset);

    // 目前前的阻塞标记为，现在把 SIGUSR2 删掉
    // 0000000001000000000000000000000000000000000000000000000000000000
    sigemptyset(&bset); // 清空信号集
    sigaddset(&bset, SIGUSR2);
    sigprocmask(SIG_SETMASK, &new_set_overwrite, &oset);


    pause();
    return 0;
}

/*
1. 默认阻塞信号集没有设置，全部不阻塞。
2. 系统调用时，要小心使用信号
*/