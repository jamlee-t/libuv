#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>

// bind 和 connect 在 udp 客户端是有效的

// 用send()函数发送数据的udp客户端程序
int main(int argc, char *argv[])
{
    int sd;
    struct sockaddr_in svr_addr;
    int ret;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    char buf[BUFSZ] = {};

    if ((sd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    //先调用connect()函数，为套接字指定目的地址/端口
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_port = htons(PORT);
    svr_addr.sin_addr.s_addr = inet_addr("192.168.1.166");
    connect(sd, (struct sockaddr* )&svr_addr, addrlen);

    while (1)
    {   
        memset(buf, 0, BUFSZ);
        printf("ple input: ");
        fgets(buf, BUFSZ, stdin);
        //sendto(sd, buf, BUFSZ, 0, (struct sockaddr* )&svr_addr, addrlen);
        send(sd, buf, BUFSZ, 0);

        ret = recvfrom(sd, buf, BUFSZ, 0, (struct sockaddr* )&svr_addr, &addrlen);
        printf("client: IPAddr = %s, Port = %d, buf = %s\n", inet_ntoa(svr_addr.sin_addr), ntohs(svr_addr.sin_port), buf);  
    }

    close(sd);  
    return 0;
}

//为客户端绑定端口和地址信息
int main2(int argc, char *argv[])
{
    int sd;
    struct sockaddr_in svr_addr, cli_addr;
    int ret;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    char buf[BUFSZ] = {};

    if ((sd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    //绑定地址信息
    cli_addr.sin_family = AF_INET;
    cli_addr.sin_port = htons(9693);
    cli_addr.sin_addr.s_addr = 0;
    if ((ret = bind(sd, (struct sockaddr* )&cli_addr, addrlen)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    svr_addr.sin_family = AF_INET;
    svr_addr.sin_port = htons(PORT);
    svr_addr.sin_addr.s_addr = inet_addr("192.168.1.166");

    while (1)
    {   
        memset(buf, 0, BUFSZ);
        printf("ple input: ");
        fgets(buf, BUFSZ, stdin);
        sendto(sd, buf, BUFSZ, 0, (struct sockaddr* )&svr_addr, addrlen);

        ret = recvfrom(sd, buf, BUFSZ, 0, (struct sockaddr* )&svr_addr, &addrlen);
        printf("client: IPAddr = %s, Port = %d, buf = %s\n", inet_ntoa(svr_addr.sin_addr), ntohs(svr_addr.sin_port), buf);  
    }

    close(sd);

    return 0;
}