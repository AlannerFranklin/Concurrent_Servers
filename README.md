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

## 3. 客户端测试脚本

*   **脚本**: `simple_client.py`
*   **用法**:
    ```bash
    python3 simple_client.py localhost 9090
    ```

## 3. 学习心得与坑点
*   **Socket API**: 理解了 `socket`, `bind`, `listen`, `accept`, `recv`, `send` 的基本流程。
*   **多线程陷阱**: 在创建线程时，传递给线程的参数（如 `sockfd`）必须是堆内存 (`malloc`) 分配的，不能传栈变量的地址，否则会有 Race Condition。
