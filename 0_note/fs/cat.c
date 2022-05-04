// mkdir -p ./bin && gcc 0_note/fs/cat.c  -o bin/cat  --verbose  -g -O0 -I./include/ -L./.libs -luv
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <uv.h>

void on_read(uv_fs_t *req);

uv_fs_t open_req;  // 文件的打开请求 
uv_fs_t read_req;  // 文件的读取请求 
uv_fs_t write_req; // 文件的写入请求 

static char buffer[1024];

static uv_buf_t iov;

// 异步 write 到 stdout，然后继续发起读取
void on_write(uv_fs_t *req) {
    if (req->result < 0) {
        fprintf(stderr, "Write error: %s\n", uv_strerror((int)req->result));
    }
    else {
        // 写入数据后再次触发一次读取请求，还是这个 read_req.
        uv_fs_read(req->loop, &read_req, open_req.result, &iov, 1, -1, on_read);
    }
}

void on_read(uv_fs_t *req) {
    if (req->result < 0) {
        fprintf(stderr, "Read error: %s\n", uv_strerror(req->result));
    }
    else if (req->result == 0) { // read 事件触发了，但是读入的数据为空。表示已经读取完毕。
        uv_fs_t close_req;
        // synchronous
        uv_fs_close(req->loop, &close_req, open_req.result, NULL);
    }
    else if (req->result > 0) {
        iov.len = req->result;
        // 异步 write，触发异步 read
        // uv_fs_write(req->loop, &write_req, stdout, &iov, 1, -1, on_write);
        
        // 也可以直接 print 然后再发起读取请求
        printf("%.*s", req->bufsml->len, req->bufsml->base);
        uv_fs_read(req->loop, &read_req, open_req.result, &iov, 1, -1, on_read);
    }
}

void on_open(uv_fs_t *req) {
    // The request passed to the callback is the same as the one the call setup
    // function was passed.
    assert(req == &open_req);
    if (req->result >= 0) {
        iov = uv_buf_init(buffer, sizeof(buffer));
        // 打开文件成功，加入 read 请求。这里 buf 是 1，用 pread 而不是 preadv
        uv_fs_read(req->loop, &read_req, req->result,
                   &iov, 1, -1, on_read);
    }
    else {
        fprintf(stderr, "error opening file: %s\n", uv_strerror((int)req->result));
    }
}

int main(int argc, char **argv) {
    uv_loop_t *loop = uv_default_loop();

    // 直接执行 open。传入 1 个 open 请求。
    uv_fs_open(loop, &open_req, argv[1], O_RDONLY, 0, on_open);

    uv_run(loop, UV_RUN_DEFAULT);

    // 清理3个用到的 uv_fs_t request
    uv_fs_req_cleanup(&open_req);
    uv_fs_req_cleanup(&read_req);
    uv_fs_req_cleanup(&write_req);
    return 0;
}
