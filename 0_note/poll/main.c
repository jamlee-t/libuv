#include  <unistd.h>
#include  <sys/types.h>       /* basic system data types */
#include  <sys/socket.h>      /* basic socket definitions */
#include  <netinet/in.h>      /* sockaddr_in{} and other Internet defns */
#include  <arpa/inet.h>       /* inet(3) functions */
 
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
 
#include <poll.h> /* poll function */
#include <limits.h>
 
#define MAXLINE 10240
 
#ifndef OPEN_MAX
#define OPEN_MAX 40960
#endif

void error(char *msg) {
  perror(msg);
  exit(1);
}

int
main(int argc, char **argv)
{
    struct linger l = { 1, 0 };

    int                 i, maxi, listenfd, connfd, sockfd;
    int                 nready;
    ssize_t             n;
    char                buf[MAXLINE];
    socklen_t           clilen;
    struct pollfd       client[OPEN_MAX];
    struct sockaddr_in  cliaddr, servaddr;
    char *              ip;
 
    // 创建 listend socket 文件描述符，并且绑定地址
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(6888);

    int optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    // 这里必须报错，显示 Address in use
    if (bind(listenfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
        error("ERROR on binding");
    }
    listen(listenfd, 1024); // backlog 1024
 
    // poll 的 fds 参数统统先赋值为 -1
    client[0].fd = listenfd;
    client[0].events = POLLRDNORM; // Normal data may be read without blocking. man poll 可以查询
    for (i = 1; i < OPEN_MAX; i++)
        client[i].fd = -1;      /* -1 indicates available entry */
    maxi = 0;                   /* max index into client[] array */

    for ( ; ; ) {
        nready = poll(client, maxi+1, -1); // maxi 表示client数组大小
        // nready 是已 ready 事件的数量。
        if (client[0].revents & POLLRDNORM) {
            clilen = sizeof(cliaddr);
            connfd = accept(listenfd, (struct sockaddr *) &cliaddr, &clilen); // accept函数返回了一个socketfd

            // 设置 fd 选项，SO_LINGER, 如果发送缓存区有数据, 立即 reset。
            // setsockopt(connfd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
            
            ip = inet_ntoa(cliaddr.sin_addr);
            printf("new client: %s:%d, fd is %d\n", ip, cliaddr.sin_port, clilen);
            for (i = 1; i < OPEN_MAX; i++) // 监视connfd是否可读、可写
                if (client[i].fd < 0) {
                    client[i].fd = connfd;  /* save descriptor */
                    break;
                }
            if (i == OPEN_MAX)
                // err_quit("too many clients");
                printf("too many clients\n");
 
            client[i].events = POLLRDNORM; // 检测connfd是否可读
            if (i > maxi)
                maxi = i;                  /* max index in client[] array */
 
            if (--nready <= 0)/* 如果除了listen的client[0]被激活，其他事件没有没有被激活则nready是1
                * 自减1后，为0，表示此次处理poll结束。继续下次监视。
                */
                continue;               /* no more readable descriptors */
        }
        for (i = 1; i <= maxi; i++) {    /* 第0个元素是处理listen的，处理其余accept的所有可读的connfd */
            if ( (sockfd = client[i].fd) < 0)// 无效的fd
                continue;
            if (client[i].revents & (POLLRDNORM | POLLERR)) {//处理可读的connfd
                if ( (n = read(sockfd, buf, MAXLINE)) < 0) {
                    if (errno == ECONNRESET) {
                            /* connection reset by client */
                        printf("client[%d] aborted connection\n", i);
                        close(sockfd);
                        client[i].fd = -1;
                    } else
                        printf("read error");
                        // err_sys("read error");
                } else if (n == 0) {
                    /* connection closed by client */
                    printf("client[%d] closed connection\n", i);
                    close(sockfd);
                    client[i].fd = -1;
                } else
                    //writen(sockfd, buf, n);
                    write(sockfd, buf, n);
 
                if (--nready <= 0)
                    break;   /* no more readable descriptors */
            }
        }
    }
}