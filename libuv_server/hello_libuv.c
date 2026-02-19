#include <stdio.h>
#include <stdlib.h>
#include <uv.h>

int main() {
    // 1. 创建 Event Loop
    // uv_default_loop() 返回默认的事件循环单例
    uv_loop_t *loop = uv_default_loop();

    printf("Hello Libuv! Event Loop is ready.\n");
    printf("Libuv version: %s\n", uv_version_string());

    // 2. 运行 Event Loop
    // uv_run 是一个阻塞调用，直到所有注册的事件都处理完毕
    // 如果没有注册任何事件（如定时器、IO），它会立即返回
    uv_run(loop, UV_RUN_DEFAULT);

    // 3. 清理资源
    uv_loop_close(loop);
    return 0;
}
