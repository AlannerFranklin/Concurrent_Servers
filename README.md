# Concurrent Servers 学习笔记

本项目是跟随 Eli Bendersky 的教程 [Concurrent Servers](https://eli.thegreenplace.net/2017/concurrent-servers-part-1-introduction/) 进行的学习实践。

目标是探索不同的并发服务器模型，从简单的顺序服务器到多线程、事件驱动等高级模型。

## 1. 协议规范 (Protocol)

这是一个简单的有状态协议 (Stateful Protocol)。

### 状态机
1. **连接建立**: 
   - 服务端接受连接。
   - **服务端** -> 发送 `*` 给客户端。
   - 服务端进入 `WAIT_FOR_MSG` 状态。

2. **WAIT_FOR_MSG 状态**:
   - 服务端忽略客户端发送的所有字符，直到收到 `^`。
   - 收到 `^` (由**客户端**发送) -> 服务端进入 `IN_MSG` 状态。

3. **IN_MSG 状态**:
   - 服务端回显 (Echo) 客户端发送的每一个字符，但会将字节值 `+1`。
   - 例如：客户端发 `A` (ASCII 65)，服务端回 `B` (ASCII 66)。
   - 收到 `$` (由**客户端**发送) -> 服务端回到 `WAIT_FOR_MSG` 状态。
   - 注意：`^` 和 `$` 仅作为分隔符，**不会**被回显。

## 2. 进度与编译指南

### 2.1 顺序服务器 (Sequential Server)
*   **代码位置**: `sequential_server/`
*   **特点**: 一次只能服务一个客户端，必须等当前客户端断开连接后才能服务下一个。
*   **编译**:
    ```bash
    gcc sequential_server/sequential_server.c sequential_server/utils.c -o sequential_server_bin
    ```
*   **运行**:
    ```bash
    ./sequential_server_bin
    ```

### 2.2 多线程服务器 (Threaded Server)
*   **代码位置**: `threads/`
*   **特点**: 为每个客户端创建一个新线程 (Thread-per-client)。可以同时服务多个客户端。
*   **编译**:
    ```bash
    gcc threads/threaded_server.c sequential_server/utils.c -o threaded_server_bin -pthread
    ```
*   **运行**:
    ```bash
    ./threaded_server_bin
    ```
    *验证*: 开启两个终端分别运行客户端，可以看到它们互不干扰。

### 2.3 线程池服务器 (Thread Pool Server)
*   **代码位置**: `thread_pool/`
*   **特点**: 预先创建固定数量的线程（如 4 个），通过任务队列分发连接。避免了频繁创建/销毁线程的开销，防止系统过载。
*   **核心技术**:
    *   **生产者-消费者模型**: 主线程 Accept -> 入队 -> Worker 线程抢锁 -> 出队 -> 处理。
    *   **条件变量**: `pthread_cond_wait` 实现“无任务睡眠，有任务唤醒”。
    *   **循环队列**: 使用 `% queue_size` 实现固定内存的循环复用。
    *   **坑点修复**: 必须 `malloc` 新内存来传递 `sockfd` 参数，防止主线程修改变量导致 Race Condition。
*   **编译**:
    ```bash
    gcc thread_pool/thread_pool_server.c thread_pool/thread_pool.c sequential_server/utils.c -o thread_pool/server -pthread
    ```
*   **运行**:
    ```bash
    ./thread_pool/server
    ```

### 2.4 IO 多路复用服务器 (Select Server)
*   **代码位置**: `select_server/`
*   **特点**: **单线程**实现并发。利用 OS 提供的 `select` 系统调用，同时监控多个 Socket 的状态（可读/可写）。
*   **核心技术**:
    *   **非阻塞 IO (Non-blocking IO)**: 必须将所有 Socket 设为非阻塞，防止某个客户端卡死整个线程。
    *   **状态机 (State Machine)**: 因为无法在一个循环里等待完整消息，必须维护每个客户端的 `state` (INITIAL_ACK / WAIT_FOR_MSG / IN_MSG)，逐字节处理。
    *   **输出缓冲区**: `send` 也可能阻塞，所以需要维护 `send_buf`，并监听 `writefds`，在 Socket 可写时再发送。
*   **编译**:
    ```bash
    gcc select_server/select_server.c sequential_server/utils.c -o select_server/server
    ```
*   **运行**:
    ```bash
    ./select_server/server
    ```

### 2.5 Epoll 服务器 (Epoll Server)
*   **代码位置**: `epoll_server/`
*   **特点**: Linux 特有的高性能 IO 复用模型。解决了 Select 的 O(N) 轮询性能问题。
*   **核心优势**:
    *   **O(k) 效率**: 仅处理活跃的 Socket，无需遍历所有连接。
    *   **边缘触发/水平触发**: 本实现使用默认的水平触发 (Level Triggered)。
    *   **动态监听**: 只有在有数据要发送时才开启 `EPOLLOUT` 监听，避免不必要的内核唤醒。
*   **编译**:
    ```bash
    gcc epoll_server/epoll_server.c sequential_server/utils.c -o epoll_server/server
    ```
*   **运行**:
    ```bash
    ./epoll_server/server
    ```

### 2.6 Libuv 服务器 (Libuv Server)
*   **代码位置**: `libuv_server/`
*   **特点**: 使用 Libuv 库（Node.js 底层）实现跨平台异步 IO。
    *   **事件驱动**: 不再直接操作 fd，而是使用 Handles 和 Streams。
    *   **回调地狱 (Callback Hell)**: 业务逻辑被拆分到 `on_connect`, `on_read`, `on_write` 等回调函数中。
    *   **状态管理**: 利用 `client->data` 指针在不同回调之间传递上下文 (Context)。
*   **核心技术**:
    *   **Work Queue**: 利用线程池处理耗时任务（如文件 IO），避免阻塞主事件循环。
    *   **State Machine**: 在回调中维护协议状态 (`WAIT_FOR_MSG` / `IN_MSG`)。
*   **编译**:
    ```bash
    gcc libuv_server/libuv_server.c sequential_server/utils.c -o libuv_server/server -luv
    ```
*   **运行**:
    ```bash
    ./libuv_server/server
    ```

### 2.7 Redis 案例研究 (Redis Case Study)
*   **学习内容**: 深入分析 Redis 的高性能架构 (Part 5)。
*   **核心架构**:
    *   **单线程事件循环**: 避免了多线程的锁竞争和上下文切换开销。
    *   **ae 库**: Redis 自研的简单事件库，封装了 `epoll`/`kqueue`/`select`。
    *   **写优化 (beforeSleep)**: 不立即调用 `send`，而是将待发送数据放入链表，在每次进入 `epoll_wait` 睡眠**之前**批量发送，减少系统调用次数。
*   **启示**: 
    *   高性能不一定需要多线程。对于 IO 密集型 + 内存操作，单线程往往更快。
    *   **CPU 瓶颈**: 单线程模型的最大弱点是不能有耗时命令（如 `KEYS *`），否则会阻塞整个服务。

### 2.8 性能测试 (Benchmark)
*   **代码位置**: `benchmark.go`
*   **语言**: Go (Golang)
*   **目的**: 使用 Go 语言的高并发特性 (Goroutines) 对上述所有 C 服务器进行压力测试，验证其稳定性与性能极限。
*   **功能**:
    *   **并发连接**: 模拟数千个客户端同时连接。
    *   **协议兼容**: 实现了本项目的自定义协议 (握手 `*` -> 发送 `^` + Payload -> 接收回显)。
    *   **统计指标**: 实时计算 QPS (Queries Per Second)、延迟分布 (P50/P99) 和错误率。
    *   **可视化**: 在终端输出简单的 ASCII 延迟直方图。
*   **运行**:
    ```bash
    go run benchmark.go -c 100 -d 10s -addr localhost:9090
    ```
    (参数说明: `-c` 并发数, `-d` 测试时长, `-addr` 目标地址)

## 3. 客户端测试脚本

*   **脚本**: `simple_client.py`
*   **用法**:
    ```bash
    python3 simple_client.py localhost 9090
    ```

## 4. 局限性与 Future Work

### 4.1 当前实现的局限性 (Limitations)
*   **Select 的连接数限制**: `select_server` 受限于 `FD_SETSIZE` (通常是 1024)，无法支持超大规模并发。
*   **内存拷贝开销**: 目前所有的 Buffer 读写都涉及用户态到内核态的拷贝，尚未利用 Zero-copy 技术（如 `sendfile`）。
*   **单线程瓶颈**: 虽然 IO 复用解决了并发连接数问题，但所有业务逻辑仍在**单线程**运行。如果业务计算密集（例如加密解密、复杂数据处理），会阻塞整个事件循环，导致所有客户端延迟增加。
*   **惊群效应**: 虽然目前未实现多进程 Epoll，但如果将来扩展，简单的 Accept 可能会遇到惊群问题。
*   **固定 Buffer 大小**: 使用了固定的 `1024` 字节 Buffer，对于超长消息可能会有截断或处理复杂性。
*   **Epoll 状态存储查找慢**: 目前 `epoll_server` 仍然使用简单的数组 (`clients[MAX_EVENTS]`) 来通过 fd 查找客户端状态。如果 fd 值很大（超过 1024），会导致数组越界或浪费内存。在生产环境中，应该使用**哈希表 (Hash Map)** 来存储 `fd -> client_state` 的映射，以便高效且节省内存地管理稀疏连接。

### 4.2 未来改进方向 (Future Work)
*   **Reactor 模式**: 封装更通用的 Reactor 库，将 IO 事件与具体的业务回调解耦（类似 libevent/libuv）。
*   **多线程 + Event Loop (One Loop Per Thread)**: 结合多线程和 Epoll。主线程只负责 Accept，然后将新连接分发给 Worker 线程的 Event Loop。这是 Nginx 和 Netty 的核心模型，能充分利用多核 CPU。
*   **超时管理**: 目前没有处理僵尸连接。应该增加定时器轮或者红黑树来管理连接超时，踢掉长时间不活动的客户端。
*   **应用层 Buffer 优化**: 实现动态扩容的 RingBuffer，替代目前简单的定长数组。
*   **Node.js & Promises (Part 6)**: 学习 Node.js 的异步编程模型，对比 C/C++ 的实现差异。

## 5. 学习心得与坑点
*   **Socket API**: 理解了 `socket`, `bind`, `listen`, `accept`, `recv`, `send` 的基本流程。
*   **多线程陷阱**: 在创建线程时，传递给线程的参数（如 `sockfd`）必须是堆内存 (`malloc`) 分配的，不能传栈变量的地址，否则会有 Race Condition。
*   **Select 的坑**: 
    1. 必须使用**非阻塞 IO**。
    2. `send` 不能直接调，要配合 `writefds` 和缓冲区，否则会丢数据。
    3. `FD_SET` 必须每次循环都重置，因为 `select` 会修改传入的集合。
*   **Epoll 思考**: 理解了为何 Select 是 O(N) 而 Epoll 是 O(k)。Epoll 通过事件回调机制，只返回就绪的 FD，在大并发下性能优势巨大。
