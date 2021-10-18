#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    pid_t pid;
    char *message;
    int n;
    printf("start working \n");
    pid = fork();
    if(pid < 0)
    {
        perror("fork failed");
        exit(1);
    }
    if(pid == 0)
    {
        printf("This is the child process. My PID is: %d. My PPID is: %d.\n", getpid(), getppid());
    }
    else
    {
        printf("This is the parent process. My PID is %d.\n", getpid());
    }
    return 0;
}
