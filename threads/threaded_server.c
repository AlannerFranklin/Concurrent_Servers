#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../utils.h"

typedef struct { int sockfd;} thread_config_t;

typedef enum { WAIT_FOR_MSG, IN_MSG } ProcessingState;

void serve_connection(int sockfd) {
    if (send(sockfd, "*", 1, 0) < 1) {
        perror_die("send");
    }
    ProcessingState state = WAIT_FOR_MSG;
    while(1) {
        uint8_t buf[1024];
        int len = recv(sockfd, buf, sizeof buf, 0);

        if (len < 0) {
            perror_die("recv");
        } else if (len == 0) {
            break;
        }

        for (int i = 0;i < len;i++) {
            switch(state) {
                case WAIT_FOR_MSG:
                    if (buf[i] == '^') {
                        state = IN_MSG;
                    }
                    break;
                case IN_MSG:
                    if (buf[i] == '$') {
                        state = WAIT_FOR_MSG;
                    } else {
                        buf[i] += 1;
                        if(send(sockfd, &buf[i], 1, 0) < 1) {
                            perror("send error");
                            close(sockfd);
                            break;
                        }
                    }
                    break;
            }
        }
    }
    close(sockfd);
}

void* server_thread(void *arg) {
    thread_config_t* config = (thread_config_t*) arg;
    int sockfd = config->sockfd;
    free(config);

    unsigned long id = (unsigned long)pthread_self();
    printf("Thread %lu created to handle connection with socket %d\n", id, sockfd);
    
    serve_connection(sockfd);
    printf("Thread %lu done\n", id);
    return 0;
}

/* 为什么我们要用 malloc 给 config 分配内存？能不能直接传 &newsockfd ？
- newsockfd 是 main 函数里的一个局部变量。
- 主线程跑得飞快，它马上就会去 accept 下一个连接， 修改 newsockfd 的值。
- 如果新线程启动得稍微慢一点（这是常态），等它去读 &newsockfd 的时候，
这个变量可能已经被主线程改成下一个客户的 socket 了！
- 结果 ：新线程接待了错误的客户，或者两个线程在抢同一个客户。
- 解决 ：必须用 malloc 复制一份数据，
把所有权完全交给新线程 ( free(config) 由新线程负责)。*/

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int portnum = 9090;
    if (argc >= 2) {
        portnum = atoi(argv[1]);
    }
    printf("Serving on port %d\n", portnum);
    fflush(stdout);//强制把缓冲区里的内容打印到屏幕上 。

    int sockfd = listen_inet_socket(portnum);//以9090进行监听，不是广播

    while(1) {
        struct sockaddr_in peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        /*接受连接请求，表示连通了 */
        int newsockfd = accept(sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);
        if (newsockfd < 0) {
            perror_die("ERROR on accept");
        }
        report_peer_connected(&peer_addr, peer_addr_len);
        pthread_t the_thread;
        thread_config_t* config = (thread_config_t*)malloc(sizeof*(config));
        if (!config) {
            die("OOM");
        }
        config->sockfd = newsockfd;
        pthread_create(&the_thread, NULL, server_thread, config);
        pthread_detach(the_thread);
    }
    return 0;
}
