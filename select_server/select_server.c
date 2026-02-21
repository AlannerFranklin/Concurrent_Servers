#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/select.h>
#include "../utils.h"
#include <string.h>

#if 0
// 宏定义：select 最多能监控 FD_SETSIZE (通常是 1024) 个 socket
#define MAX_CLIENTS 100

int main(int argc, char** argv) {
    // 设置端口
    setvbuf(stdout, NULL, _IONBF, 0); //printf立即输出，方便调试
    int portnum = 9090;
    if (argc >= 2) {
        portnum = atoi(argv[1]);
    }
    printf("Serving on port %d\n", portnum);
    
    // 启动监听socket
    int listener_sockfd = listen_inet_socket(portnum);

    // 关键点：一定要把 listener 设为非阻塞！
    // 否则如果 select 告诉我有人连接，但我 accept 的时候对方正好断网了，
    // 我就会卡在 accept 这里，导致整个服务器卡死。
    make_socket_non_blocking(listener_sockfd);

    // 准备select需要的监控名单fd_set
    fd_set readfds; //位图bitmap，每一位代表一个socket
    int max_fd = listener_sockfd; // select 监控的最大 socket 描述符
      
    int client_sockets[MAX_CLIENTS]; // 记录所有客户端的 socket 描述符
    for (int i = 0;i < MAX_CLIENTS; i++) {
        client_sockets[i] = 0;
    }

    while (1) {
        // 每次循环都要清空readfds
        FD_ZERO(&readfds);

        // listener加入监控名单(用来检测有没有新连接)
        FD_SET(listener_sockfd, &readfds);
        max_fd = listener_sockfd;

        // 把所有已连接的客户端socket也加入监控名单
        for (int i = 0;i < MAX_CLIENTS; i++) {
            int fd = client_sockets[i];
            if (fd > 0) {
                FD_SET(fd, &readfds);
                if (fd > max_fd) {
                    max_fd = fd;
                }
            }
        }
        
        // 调用select监控所有socket
        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            perror_die("select error");
            continue;
        }

        // 检查是不是有新连接
        if (FD_ISSET(listener_sockfd, &readfds)) {
            struct sockaddr_in peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);
            int new_socket = accept(listener_sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);

            if (new_socket < 0) {
                perror("accept error");
            } else {
                /* 防止网络欺诈或者竞争条件，
                新连接的socket设为非阻塞*/
                make_socket_non_blocking(new_socket);
                printf("New connection, socket fd is %d\n", new_socket);
                // 把新连接的socket加入client_sockets数组
                for (int i = 0;i < MAX_CLIENTS; i++) {
                    if (client_sockets[i] == 0) {
                        client_sockets[i] = new_socket;
                        printf("Adding to list of sockets as %d\n", i);
                        break;
                    }
                }
            }
        }

        // 检查是不是有已连接的客户端有数据可读
        for (int i = 0;i < MAX_CLIENTS; i++) {
            int sockfd = client_sockets[i];
            // 如果socket在名单中并且有消息
            if (FD_ISSET(sockfd, &readfds)) {
                char buffer[1024];
                int valread = recv(sockfd, buffer, 1024, 0);

                if (valread == 0) {
                    // 如果是0说明断开连接
                    struct sockaddr_in addr;
                    socklen_t addr_len = sizeof(addr);
                    getpeername(sockfd, (struct sockaddr*)&addr, &addr_len);
                    printf("Host disconnected, fd %d\n", sockfd);
                    close(sockfd);
                    client_sockets[i] = 0;
                }   
                else {
                    // 处理客户端消息
                    buffer[valread] = '\0';
                    send(sockfd, buffer, valread, 0);
                }
            }
        }
    }
    return 0;
}
#endif


#define MAX_CLIENTS 100
#define SENDBUF_SIZE 1024
// 定义协议状态
typedef enum {
    INITIAL_ACK,  // 刚连上，还没发送 '*'
    WAIT_FOR_MSG, // 等待消息开始符 '^'
    IN_MSG        // 正在接收消息，等待结束符 '$'
} ProcessingState;

// 定义每个客户端的状态
typedef struct {
    int fd;
    ProcessingState state;
    // 缓冲区
    char buf_to_send[SENDBUF_SIZE];
    int bytes_to_send;// 缓冲区里有多少数据待发送
} client_state_t;

// 初始化客户端状态数组
client_state_t clients[MAX_CLIENTS];

void init_clients() {
    for (int i = 0;i < MAX_CLIENTS; i++) {
        clients[i].fd = -1; // -1表示空位
        clients[i].state = INITIAL_ACK;
        clients[i].bytes_to_send = 0;
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int portnum = 9090;
    if (argc >= 2) {
        portnum = atoi(argv[1]);
    }
    printf("Serving on port %d\n", portnum);

    int listener_sockfd = listen_inet_socket(portnum);
    // 关键点：一定要把 listener 设为非阻塞！
    // 否则如果 select 告诉我有人连接，但我 accept 的时候对方正好断网了，
    // 我就会卡在 accept 这里，导致整个服务器卡死。
    make_socket_non_blocking(listener_sockfd);

    init_clients();
    fd_set readfds, writefds;
    int max_fd = listener_sockfd;

    while (1) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(listener_sockfd, &readfds);
        max_fd = listener_sockfd;

        // 将所有有效的客户端 fd 加入 select 监控集合
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &readfds);
                
                // 只有当有数据要发送时，才加入 writefds
                if (clients[i].bytes_to_send > 0) {
                    FD_SET(clients[i].fd, &writefds);
                } else if (clients[i].state == INITIAL_ACK) {
                    // 对于新连接，我们立即准备发送 '*'
                    if (clients[i].bytes_to_send < SENDBUF_SIZE) {
                         clients[i].buf_to_send[clients[i].bytes_to_send++] = '*';
                         clients[i].state = WAIT_FOR_MSG;
                         // 加入 writefds 以便立即发送
                         FD_SET(clients[i].fd, &writefds);
                    }
                }

                if (clients[i].fd > max_fd) {
                    max_fd = clients[i].fd;
                }
            }
        }

        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);

        if (activity < 0) {
            perror("select error");
            continue;
        }

        // 处理新连接
        if (FD_ISSET(listener_sockfd, &readfds)) {
            struct sockaddr_in peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);
            int new_socket = accept(listener_sockfd, (struct sockaddr *)&peer_addr, &peer_addr_len);

            if (new_socket < 0) {
                perror("accept error");
            } else {
                make_socket_non_blocking(new_socket);
                printf("New connection, socket fd is %d\n", new_socket);
                for (int i = 0;i < MAX_CLIENTS; i++) {
                    // 如果是-1状态，就变成就绪态
                    if (clients[i].fd == -1) {
                        clients[i].fd = new_socket;
                        clients[i].state = INITIAL_ACK;
                        clients[i].bytes_to_send = 0;
                        printf("Adding to list of clients at index %d\n", i);
                        break;
                    }
                }
            }
        }
        // 检查是不是有已连接的客户端有数据可读
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd == -1) {
                continue;
            }
            int sockfd = clients[i].fd;

            if (FD_ISSET(sockfd, &readfds)) {
                char buffer[1024];
                int valread = recv(sockfd, buffer, sizeof(buffer), 0);
                
                if (valread <= 0) {
                    // 客户端断开或出错
                    if (valread == 0) {
                        // 正常关闭
                        printf("Host disconnected, fd %d\n", sockfd);
                    } else {
                        perror("recv error");
                    }
                    close(sockfd);
                    FD_CLR(sockfd, &readfds); // 确保从集合中移除
                    clients[i].fd = -1; // 释放位置
                    clients[i].bytes_to_send = 0;
                    clients[i].state = INITIAL_ACK;
                    continue; // 处理下一个客户端
                }

                // 处理接收到的数据
                for (int k = 0; k < valread; k++) {
                    char input = buffer[k];
                    switch (clients[i].state) {
                        case INITIAL_ACK:
                            // 理论上不应该在这里收到数据，除非还没发 '*' 客户端就发数据了
                            // 这里我们简单处理，直接忽略或转入 WAIT_FOR_MSG
                            clients[i].state = WAIT_FOR_MSG;
                            // fallthrough
                        case WAIT_FOR_MSG:
                            if (input == '^') {
                                clients[i].state = IN_MSG;
                            }
                            break;
                        case IN_MSG:
                            if (input == '$') {
                                clients[i].state = WAIT_FOR_MSG;
                            } else {
                                if (clients[i].bytes_to_send < SENDBUF_SIZE) {
                                    clients[i].buf_to_send[clients[i].bytes_to_send++] = input + 1;
                                }
                            }
                            break;
                    }
                }
            }
        }

        // 处理写事件
        for (int i = 0; i < MAX_CLIENTS; i++) {
             if (clients[i].fd != -1 && clients[i].bytes_to_send > 0 && FD_ISSET(clients[i].fd, &writefds)) {
                int sockfd = clients[i].fd;
                int sent = send(sockfd, clients[i].buf_to_send, clients[i].bytes_to_send, 0);

                if (sent < 0) {
                    perror("send error");
                    close(sockfd);
                    FD_CLR(sockfd, &readfds); // 确保从集合中移除
                    clients[i].fd = -1;
                    clients[i].bytes_to_send = 0;
                    clients[i].state = INITIAL_ACK;
                    continue;
                }
                
                if (sent > 0) {
                    int remaining = clients[i].bytes_to_send - sent;
                    if (remaining > 0) {
                         memmove(clients[i].buf_to_send, clients[i].buf_to_send + sent, remaining);
                    }
                    clients[i].bytes_to_send = remaining;
                }
            }
        }
    }

    return 0;
}
