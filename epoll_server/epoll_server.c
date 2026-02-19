#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/epoll.h>
#include "../utils.h"

// Epoll 最大的事件监听数，这里决定了 epoll_wait 一次最多能返回多少个就绪事件
#define MAX_EVENTS 1024
// 发送缓冲区大小
#define SENDBUF_SIZE 1024

// 定义协议状态 (状态机)
typedef enum {
    INITIAL_ACK,  // 状态 1: 初始连接，尚未发送欢迎字符 '*'
    WAIT_FOR_MSG, // 状态 2: 等待消息开始符 '^'，在此状态下忽略所有其他输入
    IN_MSG        // 状态 3: 正在接收消息，对收到的字符 +1 回显，直到收到结束符 '$'
} ProcessingState;

// 定义每个客户端的上下文状态
// 为什么需要这个？因为在非阻塞/事件驱动模型中，我们不能在一个函数里处理完整个客户端的交互。
// 每次 recv 可能只收到一部分数据，所以我们需要保存每个客户端当前的进度 (state) 和待发送的数据 (buf_to_send)。
typedef struct {
    int fd;                 // 客户端 socket 文件描述符
    ProcessingState state;  // 当前协议状态
    char buf_to_send[SENDBUF_SIZE]; // 发送缓冲区 (用于暂时存储还没发出去的数据)
    int bytes_to_send;      // 发送缓冲区里当前有多少字节是有效的
} client_state_t;

// 全局数组：用于通过 fd (文件描述符) 快速找到对应的 client_state_t 指针
// 局限性：这里简单地用 fd 作为数组下标。因为 Linux 的 fd 是从小到大分配的整数，
// 但如果 fd 超过 1024 (MAX_EVENTS)，这个数组就会越界。
// 生产环境改进：应该使用哈希表 (HashTable) 或红黑树 (Map) 来存储 fd -> state 的映射。
client_state_t* clients[MAX_EVENTS]; 

// 初始化客户端状态数组，全部置空
void init_clients() {
    for (int i = 0; i < MAX_EVENTS; i++) {
        clients[i] = NULL;
    }
}

// 获取或创建客户端状态
// 如果是新连接，会分配内存；如果是旧连接，直接返回。
client_state_t* get_client_state(int fd) {
    // 安全检查：防止 fd 越界导致程序崩溃
    if (fd >= MAX_EVENTS) return NULL;
    
    // 如果这个 fd 还没有对应的状态对象，说明是第一次访问，进行初始化
    if (clients[fd] == NULL) {
        clients[fd] = (client_state_t*)malloc(sizeof(client_state_t));
        clients[fd]->fd = fd;
        clients[fd]->state = INITIAL_ACK; // 默认初始状态
        clients[fd]->bytes_to_send = 0;   // 初始没有数据要发
    }
    return clients[fd];
}

// 释放客户端状态内存
// 当连接断开时调用，防止内存泄漏
void free_client_state(int fd) {
    if (fd < MAX_EVENTS && clients[fd] != NULL) {
        free(clients[fd]);
        clients[fd] = NULL;
    }
}

int main(int argc, char** argv) {
    // 设置标准输出为无缓冲，方便调试信息实时显示
    setvbuf(stdout, NULL, _IONBF, 0);
    
    int portnum = 9090;
    if (argc >= 2) portnum = atoi(argv[1]);
    printf("Serving on port %d\n", portnum);

    // 创建监听 Socket (bind + listen)
    // 详细实现在 utils.c 中
    int listener_sockfd = listen_inet_socket(portnum);
    
    // 关键步骤：必须将监听 Socket 设为非阻塞
    // 否则 accept() 可能会阻塞整个线程
    make_socket_non_blocking(listener_sockfd);

    init_clients();

    // 1. 创建 epoll 实例
    // epoll_create1(0) 是较新的 API，参数 0 表示使用默认标志
    // 返回一个 epoll 文件描述符 (epfd)
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror_die("epoll_create1");
    }

    // 2. 将 listener (监听 Socket) 加入 epoll 监控
    // 我们关心的事件是 EPOLLIN (有新连接进来，相当于可读)
    struct epoll_event ev;
    ev.events = EPOLLIN; 
    ev.data.fd = listener_sockfd; // 用户数据，这里存 fd，方便后续知道是哪个 Socket 就绪
    
    // EPOLL_CTL_ADD: 添加监控事件
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener_sockfd, &ev) == -1) {
        perror_die("epoll_ctl: listener");
    }

    // 准备一个数组，用来接收 epoll_wait 返回的就绪事件
    // 只有“发生了事件”的 Socket 会被内核填入这个数组
    struct epoll_event events[MAX_EVENTS];

    while (1) {
        // 3. 等待事件发生 (核心阻塞点)
        // 参数说明：
        // epfd: epoll 实例
        // events: 用于接收结果的数组
        // MAX_EVENTS: 数组大小
        // -1: 超时时间，-1 表示无限等待，直到有事件发生
        // 返回值 n: 实际上有多少个 Socket 就绪了
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        
        if (n == -1) {
            perror("epoll_wait");
            continue;
        }

        // 4. 处理就绪事件
        // Epoll 的优势：这里只需要遍历前 n 个元素 (O(k))
        // 而 Select 必须遍历整个 FD_SET (O(N))
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // 情况 A: 监听 Socket 就绪 -> 说明有新客户端连接
            if (fd == listener_sockfd) {
                struct sockaddr_in peer_addr;
                socklen_t peer_addr_len = sizeof(peer_addr);
                int new_socket = accept(listener_sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);
                
                if (new_socket < 0) {
                    perror("accept");
                } else {
                    // 必须把新连接也设为非阻塞，否则 recv/send 会阻塞主循环
                    make_socket_non_blocking(new_socket);
                    printf("New connection, socket fd is %d\n", new_socket);
                    
                    // 将新客户端 Socket 加入 epoll 监控
                    // 初始只监听 EPOLLIN (可读)，因为刚连上还没数据要发，没必要监听 EPOLLOUT
                    struct epoll_event ev_client;
                    ev_client.events = EPOLLIN;
                    ev_client.data.fd = new_socket;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, new_socket, &ev_client) == -1) {
                        perror("epoll_ctl: add client");
                        close(new_socket);
                    } else {
                        // 初始化该客户端的状态结构体
                        get_client_state(new_socket);
                    }
                }
            } 
            // 情况 B: 普通客户端 Socket 就绪 -> 有数据读或写
            else {
                client_state_t* client = get_client_state(fd);
                if (!client) continue; // 异常保护：找不到状态则跳过

                // B.1: 处理可读事件 (EPOLLIN) -> 客户端发来了数据
                if (events[i].events & EPOLLIN) {
                    char buffer[1024];
                    
                    // 特殊逻辑：如果是刚连接 (INITIAL_ACK)，需要先发送 '*'
                    // 我们不直接 send，而是写入发送缓冲区，让后面的写事件逻辑去发送
                    if (client->state == INITIAL_ACK) {
                        if (client->bytes_to_send < SENDBUF_SIZE) {
                            client->buf_to_send[client->bytes_to_send++] = '*';
                            client->state = WAIT_FOR_MSG; // 状态流转：进入等待消息状态
                        }
                    }

                    // 尝试读取数据
                    int valread = recv(fd, buffer, 1024, 0);
                    
                    if (valread <= 0) {
                        // recv 返回 0 表示对方关闭连接，返回 -1 表示出错
                        printf("Host disconnected, fd %d\n", fd);
                        
                        // 清理工作：
                        // 1. 从 epoll 移除监控 (EPOLL_CTL_DEL)
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        // 2. 关闭 Socket
                        close(fd);
                        // 3. 释放状态内存
                        free_client_state(fd);
                        continue; // 这个客户端处理完了，跳过后面逻辑
                    } else {
                        // 收到数据，喂给状态机处理
                        for (int k = 0; k < valread; k++) {
                            char input = buffer[k];
                            switch (client->state) {
                                case INITIAL_ACK: 
                                    // 理论上不会走到这里，因为上面已经处理了
                                    break;
                                case WAIT_FOR_MSG:
                                    // 等待开始符 '^'
                                    if (input == '^') client->state = IN_MSG;
                                    break;
                                case IN_MSG:
                                    // 正在接收消息
                                    if (input == '$') {
                                        client->state = WAIT_FOR_MSG; // 收到结束符，回到等待状态
                                    } else {
                                        // 核心业务逻辑：字符 + 1，并放入发送缓冲区
                                        if (client->bytes_to_send < SENDBUF_SIZE) {
                                            client->buf_to_send[client->bytes_to_send++] = input + 1;
                                        }
                                    }
                                    break;
                            }
                        }
                    }
                }

                // B.2: 处理可写事件 (EPOLLOUT) -> 内核缓冲区空闲，可以发送数据
                // 只有当 events 包含 EPOLLOUT 时才执行
                if (events[i].events & EPOLLOUT) {
                    if (client->bytes_to_send > 0) {
                        // 尝试发送缓冲区里的数据
                        int sent = send(fd, client->buf_to_send, client->bytes_to_send, 0);
                        if (sent < 0) {
                            perror("send");
                            close(fd);
                            free_client_state(fd);
                            continue;
                        }
                        // 发送成功，更新缓冲区 (移动剩余数据到头部)
                        if (sent > 0) {
                            int remaining = client->bytes_to_send - sent;
                            memmove(client->buf_to_send, client->buf_to_send + sent, remaining);
                            client->bytes_to_send -= sent;
                        }
                    }
                }

                // 关键优化：动态调整 Epoll 监听事件 (EPOLL_CTL_MOD)
                // 为什么要这样做？
                // 如果缓冲区是空的，我们不应该监听 EPOLLOUT，否则 epoll_wait 会一直立即返回 (忙轮询)，因为 Socket 通常一直是可写的。
                // 只有当 buf_to_send 里有数据时，我们才告诉内核：“我想写，请在可写时通知我”。
                struct epoll_event ev_mod;
                ev_mod.data.fd = fd;
                ev_mod.events = EPOLLIN; // 读事件永远监听
                
                if (client->bytes_to_send > 0) {
                    ev_mod.events |= EPOLLOUT; // 只有有数据发时，才追加写事件监听
                }
                
                // 更新内核中的监听规则
                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev_mod);
            }
        }
    }
    close(epfd);
    return 0;
}
