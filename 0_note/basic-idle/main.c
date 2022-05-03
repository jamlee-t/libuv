// mkdir -p ./bin && gcc 0_note/basic-idle/main.c -o bin/idle --verbose  -g -O0 -I./include/ -L./.libs -luv
#include <stdio.h>
#include <uv.h>

int64_t counter = 0;

void test_idle_cb(uv_idle_t* handle) {
    counter++;
    printf("Idling...\n");
    if (counter >= 10e6)
        uv_idle_stop(handle);
}

void test_prepare_cb(uv_prepare_t* handle) {
    counter++;
    printf("Prepareing...\n");
    if (counter >= 10e6)
        uv_idle_stop(handle);
}

void test_check_cb(uv_check_t* handle) {
    counter++;
    printf("Checking...\n");
    if (counter >= 10e6)
        uv_idle_stop(handle);
}

int main() {
    // idler 是 handle，以及自己额外的 2 个字段。idle_cb 和 queue
    uv_idle_t idler1;
    uv_idle_t idler2;
    uv_prepare_t preparer;
    uv_check_t checker;
    uv_loop_t* loop = uv_default_loop();

    // JAMLEE: 
    // 1）所谓初始化就是设置 idler 结构体，里面的字段初始化，把 idler 这个 handle 挂到 loop 中
    // idle 是 1 个handle，需要启动。wait_for_a_while 会被挂到 idle_cb 函数指针上。入参为 uv_idle_t*。
    uv_idle_init(loop, &idler1);
    uv_idle_start(&idler1, test_idle_cb);
    uv_idle_init(loop, &idler2);
    uv_idle_start(&idler2, test_idle_cb);

    uv_prepare_init(loop, &preparer);
    uv_prepare_start(&preparer, test_prepare_cb);
    
    uv_check_init(loop, &checker);
    uv_check_start(&checker, test_check_cb);

    uv_run(loop, UV_RUN_ONCE);

    uv_loop_close(loop);
    return 0;
}
