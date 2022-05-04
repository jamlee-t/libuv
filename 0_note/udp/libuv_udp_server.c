// mkdir -p ./bin && gcc 0_note/udp/libuv_udp_server.c  -o bin/libuv_udp_server  --verbose  -g -O0 -I./include/ -L./.libs -luv
// echo -n "test data" | nc -u -b 255.255.255.255 68

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>


// 为 uv_buf_t 分配数据，决定当前一次
void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

// buf 是从 io 中读取的 buf
void on_read(uv_udp_t *req, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
    printf("hello world dhcp request\n");
    if (nread < 0) {
        fprintf(stderr, "Read error %s\n", uv_err_name(nread));
        uv_close((uv_handle_t*) req, NULL);
        free(buf->base);
        return;
    }

    char sender[17] = { 0 };
    uv_ip4_name((const struct sockaddr_in*) addr, sender, 16);
    fprintf(stderr, "Recv from %s\n", sender);

    // ... DHCP specific code
    unsigned int *as_integer = (unsigned int*)buf->base;
    unsigned int ipbin = ntohl(as_integer[4]);
    unsigned char ip[4] = {0};
    int i;
    for (i = 0; i < 4; i++)
        ip[i] = (ipbin >> i*8) & 0xff;
    fprintf(stderr, "Offered IP %d.%d.%d.%d\n", ip[3], ip[2], ip[1], ip[0]);

    free(buf->base); // 数据清空
    uv_udp_recv_stop(req); // 关闭 on_read 的 request
}

// 请求报文：DHCP Discover
uv_buf_t make_discover_msg() {
    uv_buf_t buffer;
    alloc_buffer(NULL, 256, &buffer);
    memset(buffer.base, 0, buffer.len);

    // BOOTREQUEST
    buffer.base[0] = 0x1;
    // HTYPE ethernet
    buffer.base[1] = 0x1;
    // HLEN
    buffer.base[2] = 0x6;
    // HOPS
    buffer.base[3] = 0x0;
    // XID 4 bytes
    buffer.base[4] = (unsigned int) random();
    // SECS
    buffer.base[8] = 0x0;
    // FLAGS
    buffer.base[10] = 0x80;
    // CIADDR 12-15 is all zeros
    // YIADDR 16-19 is all zeros
    // SIADDR 20-23 is all zeros
    // GIADDR 24-27 is all zeros
    // CHADDR 28-43 is the MAC address, use your own
    buffer.base[28] = 0xe4;
    buffer.base[29] = 0xce;
    buffer.base[30] = 0x8f;
    buffer.base[31] = 0x13;
    buffer.base[32] = 0xf6;
    buffer.base[33] = 0xd4;
    // SNAME 64 bytes zero
    // FILE 128 bytes zero
    // OPTIONS
    // - magic cookie
    buffer.base[236] = 99;
    buffer.base[237] = 130;
    buffer.base[238] = 83;
    buffer.base[239] = 99;

    // DHCP Message type
    buffer.base[240] = 53;
    buffer.base[241] = 1;
    buffer.base[242] = 1; // DHCPDISCOVER

    // DHCP Parameter request list
    buffer.base[243] = 55;
    buffer.base[244] = 4;
    buffer.base[245] = 1;
    buffer.base[246] = 3;
    buffer.base[247] = 15;
    buffer.base[248] = 6;

    return buffer;
}

void on_send(uv_udp_send_t *req, int status) {
    if (status) {
        fprintf(stderr, "Send error %s\n", uv_strerror(status));
        return;
    }
}

// udp 因为不是连接性的。只需要两个 socket
uv_loop_t *loop;
uv_udp_t send_socket;
uv_udp_t recv_socket;

// Linux 多播编程
// 局部多播地址：在224.0.0.0～224.0.0.255之间，这是为路由协议和其他用途保留的地址，路由器并不转发属于此范围的IP包。
// 预留多播地址：在224.0.1.0～238.255.255.255之间，可用于全球范围（如Internet）或网络协议。
// 管理权限多播地址：在239.0.0.0～239.255.255.255之间，可供组织内部使用，类似于私有IP地址，不能用于Internet，可限制多播范围。
// https://blog.csdn.net/li_wen01/article/details/70048172

// Linux 广播编程(实现 DHCP)
// 广播只能在一个广播域（局域网）中传播，而不能跨网段传播。发出的数据包，局域网的所有主机都能收到这个数据包
// 广播通信由UDP实现
// 路由器不转发广播数据包。交换机会转发广播数据包
// 并非所有的计算机网络都支持广播，例如X.25网络和帧中继都不支持广播，而且也没有在“整个互联网范围中”的广播。IPv6亦不支持广播，广播相应的功能由多播代替
// 通常，广播都是限制在局域网中的，比如以太网或令牌环网络。因为广播在局域网中造成的影响远比在广域网中小得多

// 广播的IP地址(Host ID全为1)：例如192.168.43.255
// 广播的MAC地址：FF:FF:FF:FF:FF:FF
int main() {
    // 客户端属向68端口（bootps）广播请求配置，服务器向67端口（bootpc）广播回应请求。
    //////////////////////////////////////////////////////////////////////////
    // 接收端
    //////////////////////////////////////////////////////////////////////////
    loop = uv_default_loop();
    uv_udp_init(loop, &recv_socket);
    struct sockaddr_in recv_addr;
    uv_ip4_addr("0.0.0.0", 68, &recv_addr); // 使用UDP协议工作，统一使用两个IANA分配的端口：67（发送端），68（接收端）
    // 因为是 udp client，这里还需要绑定。
    uv_udp_bind(&recv_socket, (const struct sockaddr *)&recv_addr, UV_UDP_REUSEADDR);
    uv_udp_recv_start(&recv_socket, alloc_buffer, on_read); // 底层会启动 io_watcher。

    //////////////////////////////////////////////////////////////////////////
    // 发送端端。发送请求报文。本地地址 0.0.0.0:0(0表示随机端口), 目标地址 255.255.255.255
    //////////////////////////////////////////////////////////////////////////
    // 初始化 udp 发送端，底层初始化一个 socket。udp 是没有所谓的 connect 函数的，sendto 函数可以直接往 fd 上发数据
    uv_udp_init(loop, &send_socket);
    struct sockaddr_in broadcast_addr; // 广播地址 https://cloud.tencent.com/developer/article/1784577
    uv_ip4_addr("0.0.0.0", 0, &broadcast_addr); // 发送端，发送端本身必须是 0.0.0.0, 才能收到广播的回信。
    uv_udp_bind(&send_socket, (const struct sockaddr *)&broadcast_addr, 0); // 绑定到 0.0.0.0，端口 0。https://stackoverflow.com/questions/55937746/linux-what-happens-when-binding-on-port-0
    uv_udp_set_broadcast(&send_socket, 1); // SO_BROADCAST，允许发送广播数据。如果是一个广播地址，但SO_BROADCAST 选项却没有被设定， 就会返回EACCES错误。

    uv_udp_send_t send_req;
    uv_buf_t discover_msg = make_discover_msg();
    // 请求报文：DHCP Discover、DHCP Request、DHCP Release、DHCP Inform和DHCP Decline。
    // 应答报文：DHCP Offer、DHCP ACK和DHCP NAK。
    struct sockaddr_in send_addr;
    uv_ip4_addr("255.255.255.255", 67, &send_addr); 
    
    // 发送数据给广播地址。send_socket 是 uv_udp_t ，req 会关联到他。udp 底层有个 io watcher，io watcher 关联 1 个回调函数 
    uv_udp_send(&send_req, &send_socket, &discover_msg, 1, (const struct sockaddr *)&send_addr, on_send);

    return uv_run(loop, UV_RUN_DEFAULT);
}