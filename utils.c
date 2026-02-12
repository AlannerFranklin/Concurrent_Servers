#include "utils.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#define _GNU_SOURCE
#include <netdb.h>

#define N_BACKLOG 64

void die(char* fmt, ...) {
    va_list args;//指针，用来指向那些变长参数。
    va_start(args, fmt);//初始化这个指针，告诉它“变长参数从哪里开始”。
    vfprintf(stderr, fmt, args);//接受一个 va_list 作为参数
    va_end(args);//用完之后清理现场。
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

void* xmalloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        die("malloc failed");
    }
    return ptr;
}

void perror_die(char* msg) {
    perror(msg);//自动查找全局变量 errno （操作系统最近一次出错的代码），然后把它翻译成人类能看懂的英文句子打印出来。
    exit(EXIT_FAILURE);
}
void report_peer_connected(const struct sockaddr_in* sa, socklen_t salen) {
    char hostbuf[NI_MAXHOST];//用来存 对方的 IP 地址
    char portbuf[NI_MAXSERV];//用来存 对方的端口号

    if (getnameinfo((struct sockaddr*)sa, salen, hostbuf, NI_MAXHOST, portbuf, NI_MAXHOST, 0) == 0) {
        //把翻译好的 IP 填进 hostbuf ，把端口填进 portbuf
        //如果翻译成功（返回0），就打印“peer (hostname, port) connected”；如果失败，就打印“peer (unknown) connected”。
        printf("peer (%s, %s) connected\n", hostbuf, portbuf);
    } else {
        printf("peer (unknown) connected\n");
    }
}

int listen_inet_socket(int portnum) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    //创建socket实例，AF_INET表明ipv4，SOCK_STREAM表明tcp协议
    if (sockfd < 0) {
        perror_die("ERROR opening socket");
    }

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        /*如果不写这句，当你关掉服务器（倒闭）后，
        操作系统会强制保留这个端口（店面）几分钟不让别人用。
        加上这句，关掉程序后可以立刻重新运行。*/
        perror_die("setsockopt");
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;//接受任何ip
    serv_addr.sin_port = htons(portnum);
    //注明端口号，htons处理大小端字节序问题，防止 9090 被读成其他数字

    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    /*把serv_addr与sockfd绑定 */
        perror_die("ERROR on binding");
    }

    if (listen(sockfd, N_BACKLOG) < 0) {
        /*把sockfd设为监听状态，
        等待客户端连接。
        N_BACKLOG是等待队列的最大长度。此处为64*/
        perror_die("ERROR on listen");
    }

    return sockfd;
}

void make_socket_non_blocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    /*获取sockfd的文件状态标志（flags），
        并把结果存在flags变量里。
        如果失败，就打印错误信息。*/
    if (flags == -1) {
        perror_die("fcntl F_GETFL");
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        /*把sockfd设为非阻塞模式，
        这样recv就不会阻塞，
        而是返回0表示没有数据可读。*/
        perror_die("fcntl F_SETFL O_NONBLOCK");
    }
}