#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/epoll.h>
#include "../utils.h"

#define MAX_EVENTS 1024
#define SENDBUF_SIZE 1024

// 定义协议状态
typedef enum {
    INITIAL_ACK,  
    WAIT_FOR_MSG, 
    IN_MSG        
} ProcessingState;

// 定义每个客户端的状态
typedef struct {
    int fd;
    ProcessingState state;
    char buf_to_send[SENDBUF_SIZE];
    int bytes_to_send; // 缓冲区里有多少数据待发送
} client_state_t;

// 我们需要一个方式来通过 fd 找到 client_state
// 简单起见，我们还是用数组，下标直接用 fd (假设 fd 不会太大)
// 在生产环境中，这里应该用哈希表 (HashMap)
client_state_t* clients[MAX_EVENTS]; 

void init_clients() {
    // 简单起见，我们动态分配，这里先置空
    for (int i = 0; i < MAX_EVENTS; i++) {
        clients[i] = NULL;
    }
}

client_state_t* get_client_state(int fd) {
    if (fd >= MAX_EVENTS) return NULL;
    if (clients[fd] == NULL) {
        clients[fd] = (client_state_t*)malloc(sizeof(client_state_t));
        clients[fd]->fd = fd;
        clients[fd]->state = INITIAL_ACK;
        clients[fd]->bytes_to_send = 0;
    }
    return clients[fd];
}

void free_client_state(int fd) {
    if (fd < MAX_EVENTS && clients[fd] != NULL) {
        free(clients[fd]);
        clients[fd] = NULL;
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int portnum = 9090;
    if (argc >= 2) portnum = atoi(argv[1]);
    printf("Serving on port %d\n", portnum);

    int listener_sockfd = listen_inet_socket(portnum);
    make_socket_non_blocking(listener_sockfd);

    init_clients();

    // 1. 创建 epoll 实例
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror_die("epoll_create1");
    }

    // 2. 将 listener 加入 epoll 监控
    struct epoll_event ev;
    ev.events = EPOLLIN; // 监听可读事件
    ev.data.fd = listener_sockfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener_sockfd, &ev) == -1) {
        perror_die("epoll_ctl: listener");
    }

    // 准备接收事件的数组
    struct epoll_event events[MAX_EVENTS];

    while (1) {
        // 3. 等待事件发生
        // -1 表示无限等待，直到有事件
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1) {
            perror("epoll_wait");
            continue;
        }

        // 4. 只遍历发生的事件 (O(k) 效率！)
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // 情况 A: 监听 socket 有新连接
            if (fd == listener_sockfd) {
                struct sockaddr_in peer_addr;
                socklen_t peer_addr_len = sizeof(peer_addr);
                int new_socket = accept(listener_sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);
                
                if (new_socket < 0) {
                    perror("accept");
                } else {
                    make_socket_non_blocking(new_socket);
                    printf("New connection, socket fd is %d\n", new_socket);
                    
                    // 将新 socket 加入 epoll 监控
                    // 初始只监听 EPOLLIN (可读)，如果有数据要发再加 EPOLLOUT
                    struct epoll_event ev_client;
                    ev_client.events = EPOLLIN;
                    ev_client.data.fd = new_socket;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, new_socket, &ev_client) == -1) {
                        perror("epoll_ctl: add client");
                        close(new_socket);
                    } else {
                        // 初始化客户端状态
                        get_client_state(new_socket);
                    }
                }
            } 
            // 情况 B: 客户端 socket 有事件
            else {
                client_state_t* client = get_client_state(fd);
                if (!client) continue; // 异常保护

                // B.1: 可读事件 (EPOLLIN)
                if (events[i].events & EPOLLIN) {
                    char buffer[1024];
                    
                    // 先处理 INITIAL_ACK 的 '*' 发送逻辑 (放入缓冲区)
                    if (client->state == INITIAL_ACK) {
                        if (client->bytes_to_send < SENDBUF_SIZE) {
                            client->buf_to_send[client->bytes_to_send++] = '*';
                            client->state = WAIT_FOR_MSG;
                        }
                    }

                    int valread = recv(fd, buffer, 1024, 0);
                    if (valread <= 0) {
                        // 断开连接或出错
                        printf("Host disconnected, fd %d\n", fd);
                        // 从 epoll 中移除 (内核会自动移除关闭的 fd，但显式移除更安全)
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        free_client_state(fd);
                        continue;
                    } else {
                        // 状态机处理
                        for (int k = 0; k < valread; k++) {
                            char input = buffer[k];
                            switch (client->state) {
                                case INITIAL_ACK: break;
                                case WAIT_FOR_MSG:
                                    if (input == '^') client->state = IN_MSG;
                                    break;
                                case IN_MSG:
                                    if (input == '$') client->state = WAIT_FOR_MSG;
                                    else {
                                        if (client->bytes_to_send < SENDBUF_SIZE) {
                                            client->buf_to_send[client->bytes_to_send++] = input + 1;
                                        }
                                    }
                                    break;
                            }
                        }
                    }
                }

                // B.2: 可写事件 (EPOLLOUT)
                if (events[i].events & EPOLLOUT) {
                    if (client->bytes_to_send > 0) {
                        int sent = send(fd, client->buf_to_send, client->bytes_to_send, 0);
                        if (sent < 0) {
                            perror("send");
                            close(fd);
                            free_client_state(fd);
                            continue;
                        }
                        if (sent > 0) {
                            int remaining = client->bytes_to_send - sent;
                            memmove(client->buf_to_send, client->buf_to_send + sent, remaining);
                            client->bytes_to_send -= sent;
                        }
                    }
                }

                // 关键优化：根据是否有数据要发，动态调整 epoll 监听事件
                struct epoll_event ev_mod;
                ev_mod.data.fd = fd;
                ev_mod.events = EPOLLIN; // 永远监听读
                if (client->bytes_to_send > 0) {
                    ev_mod.events |= EPOLLOUT; // 有数据才监听写
                }
                // 修改 epoll 监控状态
                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev_mod);
            }
        }
    }
    close(epfd);
    return 0;
}
