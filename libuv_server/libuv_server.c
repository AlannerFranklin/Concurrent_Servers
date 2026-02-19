#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "../utils.h"

#define DEFAULT_PORT 9090
#define BACKLOG 1024
// 发送缓冲区大小
#define SEND_BUF_SIZE 1024

typedef enum {
    INITIAL_ACK,  // 状态 1: 初始连接，尚未发送欢迎字符 '*'
    WAIT_FOR_MSG, // 状态 2: 等待消息开始符 '^'，在此状态下忽略所有其他输入
    IN_MSG        // 状态 3: 正在接收消息，对收到的字符 +1 回显，直到收到结束符 '$'
} ProcessingState;

typedef struct {
    ProcessingState state;
    char sendbuf[SEND_BUF_SIZE];
    int sendbuf_end;
    uv_tcp_t* client;
} peer_state_t;

void on_wrote_buf(uv_write_t* req, int status);

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char*)xmalloc(suggested_size);
    buf->len = suggested_size;
}

void on_write(uv_write_t* req, int status) {
    if (status < 0) {
        fprintf(stderr, "Write error %s\n", uv_err_name(status));
    }
    char *base = (char*) req->data;
    free(base);
    free(req);
}
/*
void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) {
        printf("Read %zd bytes: %.*s\n", nread, (int)nread, buf->base);

        // TODO: Echo back (把数据发回去)
        // 复用 buf
        uv_buf_t write_buf = uv_buf_init(buf->base, nread);
        // 创建一个写请求 (Request)
        // 注意：每次写操作都需要一个新的 uv_write_t 结构体来追踪状态
        uv_write_t *req = (uv_write_t*)xmalloc(sizeof(uv_write_t));
        // 这里的 req->data 是一个通用指针，我们可以用来挂载任何东西
        // 这里挂载 buf->base 是为了在写完后的回调里释放它
        req->data = (void*) buf->base;
        // 发起写操作
        // 参数：请求对象, 目标流, 数据缓冲区数组, 缓冲区个数, 回调函数
        uv_write(req, client, &write_buf, 1, on_write);


    } else if (nread < 0) {
        if (nread == UV_EOF) {
            fprintf(stderr, "Read error %s\n", uv_err_name(nread));
        }
        uv_close((uv_handle_t*) client, NULL);
        if (buf->base) {
            free(buf->base);
        }
    }
}
*/

void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t* buf) {
    peer_state_t *peerstate = (peer_state_t*) client->data;

    // 探针 1: 看看有没有读到数据
    /*if (nread > 0) {
    
        printf("DEBUG: Received %zd bytes: %.*s\n", nread, (int)nread, buf->base);
    }*/

    if (nread < 0) {
        if (nread == UV_EOF) {
            fprintf(stderr, "Read error %s\n", uv_err_name(nread));
        }
        uv_close((uv_handle_t*) client, NULL);
        if (buf->base) {
            free(buf->base);
        }
        return;
    }
    if (nread == 0) {
        if (buf->base) free(buf->base);
        return;
    }
    // 状态机处理逻辑
    for (int i = 0;i < nread; ++i) {

        // 探针 2: 看看状态怎么变
        //printf("DEBUG: Char='%c' State=%d -> ", buf->base[i], peerstate->state);
        
        switch (peerstate->state) {
            case INITIAL_ACK:
                if (buf->base[i] == '*') {
                    peerstate->state = WAIT_FOR_MSG;
                }
                break;
            case WAIT_FOR_MSG:
                if (buf->base[i] == '^') {
                    peerstate->state = IN_MSG;
                }
                break;
            case IN_MSG:
                if (buf->base[i] == '$') {
                    peerstate->state = WAIT_FOR_MSG;
                } else {
                    if (peerstate->sendbuf_end >= SEND_BUF_SIZE) {
                        fprintf(stderr, "Send buffer overflow\n");
                        continue;
                    }
                    peerstate->sendbuf[peerstate->sendbuf_end++] = buf->base[i] + 1;
                }
                break;
        }
    }
    if (peerstate->sendbuf_end > 0) {
        // 探针 3: 看看是不是要发送了
        //printf("DEBUG: Sending %d bytes...\n", peerstate->sendbuf_end);

        uv_buf_t writebuf = uv_buf_init(peerstate->sendbuf, peerstate->sendbuf_end);
        uv_write_t *req = (uv_write_t*)xmalloc(sizeof(uv_write_t));
        req->data = peerstate;

        if (uv_write(req, client, &writebuf, 1, on_wrote_buf) < 0) {
            die("uv_write failed");
        }

        // 暂停读取 (Stop Reading)
        // 我现在要去发数据了，Buffer 被占用了。
        // 在发完之前 (on_wrote_buf 被调用之前)，别再给我塞新数据了！
        uv_read_stop(client);
    }
    if (buf->base) free(buf->base);
}

void on_wrote_init_ack(uv_write_t* req, int status) {
    if (status) {
        die("on_wrote_init_ack: %s", uv_err_name(status));
    }

    // req->data 里存的是 peerstate
    peer_state_t *peerstate = (peer_state_t*) req->data;

    // 注意：这里把 peerstate->client 转成 uv_stream_t*
    uv_read_start((uv_stream_t*)peerstate->client, alloc_buffer, on_read);
    free(req);
}
/*0 .

void on_peer_connected(uv_stream_t* server_stream, int status) {
    if (status < 0) {
        fprintf(stderr, "Peer connection error: %s\n", uv_strerror(status));
        return;
    }
    // 分配内存 + 初始化句柄
    // 这里必须用 malloc 分配堆内存，因为这个 client 要活很久
    uv_tcp_t* client = (uv_tcp_t*)xmalloc(sizeof(uv_tcp_t));
    uv_tcp_init(uv_default_loop(), client);

    // 把 server_stream 上的连接，转移到 client 上
    int rc;
    if ((rc = uv_accept(server_stream, (uv_stream_t*)client)) == 0) {
        printf("New client accepted!\n");

    // 读取数据
        uv_read_start((uv_stream_t*)client, alloc_buffer, on_read);
    } else {
        // 如果Accept 失败，记得释放内存
        uv_close((uv_handle_t*)client, NULL);
        fprintf(stderr, "uv_accept failed: %s\n", uv_strerror(rc));
    }
}
*/

void on_peer_connected(uv_stream_t* server_stream, int status) {
    if (status < 0) {
        fprintf(stderr, "Peer connection error: %s\n", uv_strerror(status));
        return;
    }

    uv_tcp_t* client = (uv_tcp_t*)xmalloc(sizeof(uv_tcp_t));
    uv_tcp_init(uv_default_loop(), client);

    // client->data 暂时置空，稍后挂 peerstate
    client->data = NULL;

    if(uv_accept(server_stream, (uv_stream_t*)client) == 0) {
        printf("New client accepted!\n");

        // 初始化 Peer State (协议状态)
        peer_state_t* peerstate = (peer_state_t*)xmalloc(sizeof(peer_state_t));
        peerstate->state = INITIAL_ACK;
        peerstate->sendbuf[0] = '*';
        peerstate->sendbuf_end = 1;
        peerstate->client = client;// 反向引用
        // 把 state 挂载到 client 上，方便以后随时取用
        // 上下文传递，Libuv 只会把 client (那个 uv_tcp_t* 指针) 传出来
        // on_read 被调用时，无法确定client是哪一个客户端，以及状态
        // client->data把任何关于这个客户端的信息 （比如 peer_state_t ）塞进去
        client->data = peerstate;

        uv_buf_t write_buf = uv_buf_init(peerstate->sendbuf, peerstate->sendbuf_end);
        uv_write_t *req = (uv_write_t*)xmalloc(sizeof(uv_write_t));
        req->data = peerstate;

        int rc;
        if ((rc = uv_write(req, (uv_stream_t*)client, &write_buf, 1, on_wrote_init_ack)) < 0) {
            die("uv_write: %s", uv_strerror(rc));
        }
    } else {
        uv_close((uv_handle_t*)client, NULL);
    }
}

void on_wrote_buf(uv_write_t* req, int status) {
    if (status) {
        die("Write error: %s\n", uv_strerror(status));
    }
    // 拿出上下文
    peer_state_t* peerstate = (peer_state_t*) req->data;
    // 关键点 1：重置 Buffer 指针
    // 因为写完了，Buffer 空了，下次可以从头开始写了
    peerstate->sendbuf_end = 0;
    // 关键点 2：重新开始读取 (Resume Reading)
    // 之前为了保护 Buffer，我们暂停了读取，现在可以继续了
    uv_read_start((uv_stream_t*)peerstate->client, alloc_buffer, on_read);
    // 释放请求对象 (注意：peerstate 不能释放)
    free(req);
}

int main(int argc, char **argv) {

    int portnum = DEFAULT_PORT;
    if (argc > 2) {
        portnum = atoi(argv[1]);
    }
    printf("Serving on port %d\n", portnum);

    int rc; // 用于接收返回值 (return code)
    uv_tcp_t server_stream; // 用于存储服务器的 TCP 句柄;

    // TODO: 1. Initialize TCP handle (uv_tcp_init)
    if ((rc = uv_tcp_init(uv_default_loop(), &server_stream))) {
        die("uv_tcp_init: %s", uv_strerror(rc));
    }
    // TODO: 2. Bind to address (uv_ip4_addr, uv_tcp_bind)
    struct sockaddr_in server_address;
    if ((rc = uv_ip4_addr("0.0.0.0", portnum, &server_address))) {
        die("uv_ip4_addr: %s", uv_strerror(rc));
    }
    if ((rc = uv_tcp_bind(&server_stream, (const struct sockaddr*)&server_address, 0)) < 0) {
        die("uv_tcp_bind: %s", uv_strerror(rc));
    }
    // TODO: 3. Listen for connections (uv_listen)
    if ((rc = uv_listen((uv_stream_t*)&server_stream, BACKLOG, on_peer_connected)) < 0) {
        die("uv_listen: %s", uv_strerror(rc));
    }
    // TODO: 4. Run loop (uv_run)
    printf("Server loop starting...\n");
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    
    // Cleanup
    uv_loop_close(uv_default_loop());
    return 0;
}
