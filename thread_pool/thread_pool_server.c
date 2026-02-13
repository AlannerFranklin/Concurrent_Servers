#include "thread_pool.h"
#include "../utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

void handle_client(void* arg) {
    int sockfd = *((int*)arg);
    free(arg);

    if (send(sockfd, "*", 1, 0) < 1) {
        close(sockfd);
        return;
    }

    enum {WAIT_FOR_MSG, IN_MSG} state = WAIT_FOR_MSG;
    char buf[1024];
    while (1) {
        int n = recv(sockfd, buf, sizeof buf, 0);
        if (n < 0) {
            break;
        }
        for (int i = 0;i < n;i++) {
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
                        buf[i] += 1;
                        if(send(sockfd, &buf[i], 1, 0) < 1) {
                            perror("send error");
                            close(sockfd);
                        }
                    }
                    break;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    int port = 9090;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    int listenfd = listen_inet_socket(port);
    printf("Thread Pool Server listening on port %d\n", port);

    thread_pool_t* pool = thread_pool_create(4, 100);
    if (!pool) {
        die("Failed to create thread pool");
    }
    printf("Thread pool created with 4 threads\n");

    while (1) {
        struct sockaddr_in peer_addr;
        socklen_t peer_addr_len = sizeof peer_addr;

        int newsockfd = accept(listenfd, (struct sockaddr*)&peer_addr, &peer_addr_len);
        if (newsockfd < 0) {
            perror_die("ERROR on accept");
        }
        report_peer_connected(&peer_addr, peer_addr_len);
        // 防止传递太快，修改地址，所以记下地址传给线程池
        int* arg = (int*)malloc(sizeof(int));
        *arg = newsockfd;

        thread_pool_add(pool, handle_client, arg);
    }
    thread_pool_destroy(pool);
    return 0;
}