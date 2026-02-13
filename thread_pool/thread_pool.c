#include "thread_pool.h"
#include <stdlib.h>
#include <pthread.h>
static void* thread_pool_worker(void* arg);



thread_pool_t* thread_pool_create(int thread_count, int queue_size) {
    // 1. 申请线程池管理器的内存
    thread_pool_t* pool = malloc(sizeof(thread_pool_t));

    // 2. 初始化基本参数
    pool->thread_count = thread_count;
    pool->queue_size = queue_size;
    pool->head = 0;
    pool->tail = 0;
    pool->count = 0;
    pool->shutdown = 0;

    // 3. 申请任务队列的内存
    pool->queue = (thread_task_t*)malloc(sizeof(thread_task_t) * queue_size);

    // 4. 申请线程数组的内存
    pool->threads = (pthread_t*)malloc(sizeof(pthread_t) * thread_count);

    // 5. 初始化锁和条件变量
    pthread_mutex_init(&(pool->lock), NULL);
    pthread_cond_init(&(pool->notify), NULL);

    // 6. 最关键的一步：启动所有线程！
    for (int i = 0;i < thread_count; i++) {
        // 让每个线程都去执行 thread_pool_worker 函数
        // 注意：把 pool 指针传进去，因为线程需要访问队列
        pthread_create(&(pool->threads[i]), NULL, thread_pool_worker, (void*)pool);
    }
    return pool;
}

void* thread_pool_worker(void* arg) {
    // 强制类型转换，要求传参数的类型必须是void*
    thread_pool_t* pool = (thread_pool_t*)arg;
    while (1) {
        //访问队列先加锁
        pthread_mutex_lock(&(pool->lock));

        //检测是否有任务，防止虚假唤醒
        while (pool->count == 0 && !pool->shutdown) {
            // 队列为空，等待新任务
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }
        
        if (pool->shutdown) {
            // 线程池要关闭了，退出循环
            pthread_mutex_unlock(&(pool->lock));
            pthread_exit(NULL);
        }

        // 从队列中取出任务
        thread_task_t task = pool->queue[pool->head];
        // 移动头指针，准备处理下一个任务,防止越界
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;

        // 解锁，允许其他线程访问队列
        pthread_mutex_unlock(&(pool->lock));

        // 执行任务函数，将参数传递给它
        (*(task.function))(task.argument);//相当于执行serve_connection(sockfd)
    }
}

int thread_pool_add(thread_pool_t *pool, void (*function)(void *), void* argument) {
    int err = 0;
    int next_tail;

    // 加锁
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
       return -1;
    }

    // 计算尾部位置
    next_tail = (pool->tail + 1) % pool->queue_size;

    // 检测队列是否已满
    if (pool->count == pool->queue_size) {
        err = -1;
    }

    // 检测线程池是否关闭
    if (pool->shutdown) {
        err = -1;
    } else {
        // 队列不满，添加任务
        pool->queue[pool->tail].function = function;
        pool->queue[pool->tail].argument = argument;
        pool->tail = next_tail;
        pool->count++;
        // 唤醒一个等待中的线程
        pthread_cond_signal(&(pool->notify));
    }
    pthread_mutex_unlock(&(pool->lock));
    return err;
}

int thread_pool_destroy(thread_pool_t *pool) {
    if (pool == NULL) {
        return -1;
    }
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        return -1;
    }
    pool->shutdown = 1;
    /*
    1. 先解锁，允许其他线程访问队列
    2. 广播信号，唤醒所有等待中的线程
    3. 等待所有线程执行完毕
    4. 销毁锁和条件变量
    5. 释放内存
    */
    if (pthread_mutex_unlock(&(pool->lock)) != 0 || pthread_cond_broadcast(&(pool->notify)) != 0) {
        return -1;
    }
    for (int i = 0;i < pool->thread_count;i++) {
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->queue);
    free(pool->threads);
    pthread_mutex_destroy(&(pool->lock));
    pthread_cond_destroy(&(pool->notify));
    free(pool);
    return 0;
}