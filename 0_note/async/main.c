#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <uv.h>

uv_loop_t *loop;
uv_async_t async, async2;

double percentage;

void fake_download(uv_work_t *req) {
    int size = *((int*) req->data);
    int downloaded = 0;
    while (downloaded < size) {
        percentage = downloaded*100.0/size;
        async.data = (void*) &percentage;

        // 发送给异步回调函数
        uv_async_send(&async);

        sleep(10);
        downloaded += (2+random())%1000; // can only download max 1000bytes/sec,
                                           // but at least a 200;
    }
}

void after(uv_work_t *req, int status) {
    fprintf(stderr, "Download complete\n");
    uv_close((uv_handle_t*) &async, NULL);
}

void print_progress(uv_async_t *handle) {
    double percentage = *((double*) handle->data);
    fprintf(stderr, "Downloaded %.2f%%\n", percentage);
}

void echo_hello(uv_async_t *handle) {
    printf("%s\n", "i got event too");
}

int main() {
    loop = uv_default_loop();

    uv_work_t req;
    int size = 10240;
    req.data = (void*) &size;

    // 注册 async 到 loop 中的 async_handles 上。这里异步执行的函数是 async
    // 这里没有 uv_async_start 函数。另外这个有个特殊的是 uv_async_send
    uv_async_init(loop, &async, print_progress);
    uv_async_init(loop, &async2, echo_hello);

    // req 提交给 wq。fake_download 是 worker，after 是 worker 执行完毕之后的回调
    uv_queue_work(loop, &req, fake_download, after);

    return uv_run(loop, UV_RUN_DEFAULT);
}
