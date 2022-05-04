/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* This file contains both the uv__async internal infrastructure and the
 * user-facing uv_async_t functions.
 */

// 参考资料：https://juejin.cn/post/7084612629875916808

#include "uv.h"
#include "internal.h"
#include "atomic-ops.h"

#include <errno.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>  /* sched_yield() */

#ifdef __linux__
#include <sys/eventfd.h>
#endif

static void uv__async_send(uv_loop_t* loop);
static int uv__async_start(uv_loop_t* loop);

// uv_async_t 类型的 handle。回调函数设置为 async_cb。
int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;

  err = uv__async_start(loop);
  if (err)
    return err;

  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  handle->async_cb = async_cb;
  handle->pending = 0;

  QUEUE_INSERT_TAIL(&loop->async_handles, &handle->queue);
  uv__handle_start(handle);

  return 0;
}

// 调用 aysnc handle，当前线程是线程池里的线程，发送 loop 线程。
// 1. cmpxchgi是原子操作compare_and_change。pending的有三个取值0，1，2。
// 2. loop->async_io_watcher调用uv__async_io时，会遍历loop->async_handles，通过pending来判断哪些回调该被执行。
int uv_async_send(uv_async_t* handle) {
  /* Do a cheap read first. */
  if (ACCESS_ONCE(int, handle->pending) != 0)
    return 0;

  /* https://juejin.cn/post/7084612629875916808
    设置 async handle 的 pending 标记
    如果 pending 是 0，则设置为 1，返回 0，如果是 1 则返回 1，
    所以同一个 handle 如果多次调用该函数是会被合并的
  */
  /* Tell the other thread we're busy with the handle. */
  if (cmpxchgi(&handle->pending, 0, 1) != 0) // 会有 1 个线程成功，假设为线程 A。当前只有线程A可以修改这个handle对应的pending了。
    return 0;

  /* Wake up the other thread's event loop. */
  uv__async_send(handle->loop); // 线程 A，写入 eventfd 触发事件

  // 发送完毕后，当前的 1 设置为 2.
  /* Tell the other thread we're done. */
  if (cmpxchgi(&handle->pending, 1, 2) != 1) // 线程 A，尝试将其从 1 改为 2。改成后将触发 loop 线程运行回调。
    abort();

  return 0;
}

// 只能在 loop 主进程中运行这个函数。
/* Only call this from the event loop thread. */
static int uv__async_spin(uv_async_t* handle) {
  int i;
  int rc;
  
  // 当pending被设置为2时的时刻，无限期抢
  for (;;) {
    /* 997 is not completely chosen at random. It's a prime number, acyclical
     * by nature, and should therefore hopefully dampen sympathetic resonance.
     */
    for (i = 0; i < 997; i++) {
      /* rc=0 -- handle is not pending.
       * rc=1 -- handle is pending, other thread is still working with it.
       * rc=2 -- handle is pending, other thread is done.
       */
      rc = cmpxchgi(&handle->pending, 2, 0); // 尝试把 2 变为 0，最终只有 1 个线程能够成功

      if (rc != 1) // rc == 0 或者 2 时，返回 rc
        return rc;

      /* Other thread is busy with this handle, spin until it's done. */
      cpu_relax();
    }

    /* Yield the CPU. We may have preempted the other thread while it's
     * inside the critical section and if it's running on the same CPU
     * as us, we'll just burn CPU cycles until the end of our time slice.
     */
    sched_yield();
  }
}


void uv__async_close(uv_async_t* handle) {
  uv__async_spin(handle);
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
}

// 收到 io watcher 中的可读消息后，分发、执行异步任务。loop->async_handles 中挑选 handle
static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  char buf[1024]; // 从 loop->async_io_watcher 中读到当前的消息
  ssize_t r;
  QUEUE queue;
  QUEUE* q;
  uv_async_t* h;

  assert(w == &loop->async_io_watcher);

  for (;;) {
    r = read(w->fd, buf, sizeof(buf));

    if (r == sizeof(buf))
      continue;

    if (r != -1)
      break;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

    if (errno == EINTR)
      continue;

    abort();
  }

  QUEUE_MOVE(&loop->async_handles, &queue);
  while (!QUEUE_EMPTY(&queue)) { // 异步任务注册的所有 handle
    q = QUEUE_HEAD(&queue);
    h = QUEUE_DATA(q, uv_async_t, queue);

    QUEUE_REMOVE(q);
    QUEUE_INSERT_TAIL(&loop->async_handles, q);

    // pending 是 0 时，这个 handle 事件没触发。
    // pending 是 2 时，由我这个loop执行回调。
    // 这里的意思是会等pending的状态发完消息。然后继续运行回调，当前的 handle 是 2 表示它的回调的一定待运行的。非常精妙，信号触发次数是无所谓的。
    if (0 == uv__async_spin(h)) // 这个handle是不是 pending 状态，继续
      continue;  /* Not pending. */

    if (h->async_cb == NULL)
      continue;
    // 执行异步任务回调，通过 pending 判断哪些回调会运行
    h->async_cb(h);
  }
}

// 唤醒其他 thread 中运行的 loop，调用回调函数。只管写1到异步的管道里。这不管异步处理函数是谁？
// uv_async_send(uv_async_t* handle) 包装函数，参数 handle 仅仅为了取 loop
static void uv__async_send(uv_loop_t* loop) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;

  buf = "";
  len = 1;
  fd = loop->async_wfd; // async_wfd 是 pipe[1], linux 下这个无效

#if defined(__linux__)
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    fd = loop->async_io_watcher.fd;  /* eventfd, linux下，不用 pipe 这种了 */
  }
#endif

  // 写入内容到 fd, 写入的内容只是 1 个数字 1。
  do
    r = write(fd, buf, len);
  while (r == -1 && errno == EINTR);

  // 写入成功后返回。
  if (r == len)
    return;

  if (r == -1)
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;

  abort();
}

// 启动异步 loop 中的 async handle。
static int uv__async_start(uv_loop_t* loop) {
  int pipefd[2];
  int err;

  if (loop->async_io_watcher.fd != -1)
    return 0;

#ifdef __linux__
  // eventfd() 函数会创建一个 eventfd 对象，用户空间的应用程序可以用这个 eventfd 来实现事件的等待或通知机制。
  // EFD_CLOEXEC：FD_CLOEXEC，简单说就是fork子进程时不继承，对于多线程的程序设上这个值不会有错的。
  // EFD_NONBLOCK：文件会被设置成O_NONBLOCK，一般要设置。
  // EFD_SEMAPHORE：（2.6.30以后支持）支持semophore语义的read，简单说就值递减1。 
  err = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (err < 0)
    return UV__ERR(errno);

  pipefd[0] = err; // eventfd 值是
  pipefd[1] = -1;
#else
  err = uv__make_pipe(pipefd, UV_NONBLOCK_PIPE);
  if (err < 0)
    return err;
#endif
  // 初始化 async_io_watcher, uv__io_t 类型。watch 的文件描述符是 pipefd[0]。
  uv__io_init(&loop->async_io_watcher, uv__async_io, pipefd[0]);
  uv__io_start(loop, &loop->async_io_watcher, POLLIN);
  loop->async_wfd = pipefd[1]; // eventfd 无效

  return 0;
}


int uv__async_fork(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1) /* never started */
    return 0;

  uv__async_stop(loop);

  return uv__async_start(loop);
}


void uv__async_stop(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1)
    return;

  if (loop->async_wfd != -1) {
    if (loop->async_wfd != loop->async_io_watcher.fd)
      uv__close(loop->async_wfd);
    loop->async_wfd = -1;
  }

  uv__io_stop(loop, &loop->async_io_watcher, POLLIN);
  uv__close(loop->async_io_watcher.fd);
  loop->async_io_watcher.fd = -1;
}
