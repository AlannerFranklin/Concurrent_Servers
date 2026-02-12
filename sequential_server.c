#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    #define close closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

#define PORT 9090

void die(const char *msg) {
    perror(msg);
    exit(1);
}

typedef enum { WAIT_FOR_MSG, IN_MSG } ProcessingState;

void serve_connection(SOCKET sockfd) {
    // 协议：连接建立后发送 '*'
    if (send(sockfd, "*", 1, 0) < 1) {
        die("send");
    }

    ProcessingState state = WAIT_FOR_MSG;
    char buf[1024];

    while (1) {
        int len = recv(sockfd, buf, sizeof(buf), 0);
        if (len < 0) {
            die("recv");
        } else if (len == 0) {
            break; // 客户端断开连接
        }

        for (int i = 0; i < len; ++i) {
            switch (state) {
                case WAIT_FOR_MSG:
                    if (buf[i] == '^') {
                        state = IN_MSG;
                    }
                    break;
                case IN_MSG:
                    if (buf[i] == '$') {
                        state = WAIT_FOR_MSG;
                    } else {
                        buf[i] += 1; // 简单的处理：字符 + 1
                        if (send(sockfd, &buf[i], 1, 0) < 1) {
                            die("send error");
                        }
                    }
                    break;
            }
        }
    }
    close(sockfd);
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        die("WSAStartup failed");
    }
#endif

    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        die("socket");
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
        die("setsockopt");
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        die("bind");
    }

    if (listen(server_fd, 10) < 0) {
        die("listen");
    }

    printf("Sequential server listening on port %d\n", PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        SOCKET client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd == INVALID_SOCKET) {
            die("accept");
        }

        printf("Client connected\n");
        serve_connection(client_fd);
        printf("Client disconnected\n");
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
