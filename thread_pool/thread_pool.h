#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>

// 1. 定义任务结构体
// 这里的 function 就是一个通用的函数指针
// 它可以指向任何返回值是 void，参数是 void* 的函数
// 也就是：void serve_connection(void* arg);

typedef struct {
    void (*function)(void *);
    void *argument;
} thread_task_t;

// 2. 定义线程池结构体
typedef struct {
    pthread_mutex_t lock;      // 互斥锁
    pthread_cond_t notify;     // 条件变量
    pthread_t *threads;        // 线程数组
    thread_task_t *queue;      // 任务队列数组
    int thread_count;          // 线程数量
    int queue_size;            // 队列最大长度
    int head;                  // 队头
    int tail;                  // 队尾
    int count;                 // 当前任务数
    int shutdown;              // 是否关闭
} thread_pool_t;

// 3. 函数声明
thread_pool_t* thread_pool_create(int thread_count, int queue_size);
int thread_pool_add(thread_pool_t *pool, void (*function)(void *), void *argument);
int thread_pool_destroy(thread_pool_t *pool);

#endif