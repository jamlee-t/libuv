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

#include "uv.h"
#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SA_RESTART
# define SA_RESTART 0
#endif

typedef struct {
  uv_signal_t* handle;
  int signum;
} uv__signal_msg_t;

RB_HEAD(uv__signal_tree_s, uv_signal_s);


static int uv__signal_unlock(void);
static int uv__signal_start(uv_signal_t* handle,
                            uv_signal_cb signal_cb,
                            int signum,
                            int oneshot);
static void uv__signal_event(uv_loop_t* loop, uv__io_t* w, unsigned int events);
static int uv__signal_compare(uv_signal_t* w1, uv_signal_t* w2);
static void uv__signal_stop(uv_signal_t* handle);
static void uv__signal_unregister_handler(int signum);


static uv_once_t uv__signal_global_init_guard = UV_ONCE_INIT;

// 存储信号 handle 的红黑树结构。
static struct uv__signal_tree_s uv__signal_tree =
    RB_INITIALIZER(uv__signal_tree);


// 管道的局限性：
// ① 数据自己读不能自己写。
// ② 数据一旦被读走，便不在管道中存在，不可反复读取。
// ③ 由于管道采用半双工通信方式。因此，数据只能在一个方向上流动。
// ④ 只能在有公共祖先的进程间使用管道。

// 父进程使用 fd[1]，子进程使用 fd[0]
// 调用fork后，子进程会复制父进程的进程信息，如文件描述符，这样fd[0], fd[1]在子进程中有同样的一个拷贝，他们的引用都为2，也就是两个进程在使用他们。
// 而实际上父进程只使用fd[1]，子进程只使用fd[0]，这样如果父进程不想使用fd[1]了，调用close()来关闭fd[1]，这是不成功的，
// 因为这样只是将fd[1]的引用减少到1，fd[1]没有被系统回收，仍然在子进程中有效，所以必须父进程close(fd[0]);子进程close(fd[1]);
static int uv__signal_lock_pipefd[2] = { -1, -1 };

RB_GENERATE_STATIC(uv__signal_tree_s,
                   uv_signal_s, tree_entry,
                   uv__signal_compare)

static void uv__signal_global_reinit(void);

// 启动 loop 执行一次信号全局初始化函数。
// pthread_atfork 该函数通过3个不同阶段的回调函数来处理互斥锁状态。
// int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));
static void uv__signal_global_init(void) {
  // 如果 uv__signal_lock_pipefd 还没有创建。那么在 fork 后在 child 里，再次执行 uv__signal_global_reinit。
  if (uv__signal_lock_pipefd[0] == -1)
    /* pthread_atfork can register before and after handlers, one
     * for each child. This only registers one for the child. That
     * state is both persistent and cumulative, so if we keep doing
     * it the handler functions will be called multiple times. Thus
     * we only want to do it once.
     */
    if (pthread_atfork(NULL, NULL, &uv__signal_global_reinit))
      abort();

  // 父进程中执行 1 次。
  uv__signal_global_reinit();
}

// 清理信号锁的用到的 pipe，设置为初始的 -1。关闭进程的整个信号处理模块
void uv__signal_cleanup(void) {
  /* We can only use signal-safe functions here.
   * That includes read/write and close, fortunately.
   * We do all of this directly here instead of resetting
   * uv__signal_global_init_guard because
   * uv__signal_global_once_init is only called from uv_loop_init
   * and this needs to function in existing loops.
   */
  if (uv__signal_lock_pipefd[0] != -1) {
    uv__close(uv__signal_lock_pipefd[0]);
    uv__signal_lock_pipefd[0] = -1;
  }

  if (uv__signal_lock_pipefd[1] != -1) {
    uv__close(uv__signal_lock_pipefd[1]);
    uv__signal_lock_pipefd[1] = -1;
  }
}

// loop 启动时，执行本函数，创建 pipe。
static void uv__signal_global_reinit(void) {
  uv__signal_cleanup();

  if (uv__make_pipe(uv__signal_lock_pipefd, 0))
    abort();

  // 第一次初始化时会解锁一次。这样管道内部会有1个pending的消息
  if (uv__signal_unlock())
    abort();
}

// loop 启动时执行，仅仅执行 1 次。uv_once 等同于 pthred_once。uv__signal_global_init
// int pthread_once(pthread_once_t *once_control, void (*init_routine) (void))；
void uv__signal_global_once_init(void) {
  uv_once(&uv__signal_global_init_guard, uv__signal_global_init);
}

// 子进程执行到这会阻塞，直到有 42 发送过来
static int uv__signal_lock(void) {
  int r;
  char data;

  do {
    // 只读取 1 个字符
    r = read(uv__signal_lock_pipefd[0], &data, sizeof data);
  } while (r < 0 && errno == EINTR);

  return (r < 0) ? -1 : 0;
}

// 向所有子进程发送 42
static int uv__signal_unlock(void) {
  int r;
  char data = 42;

  do {
    r = write(uv__signal_lock_pipefd[1], &data, sizeof data);
  } while (r < 0 && errno == EINTR);

  return (r < 0) ? -1 : 0;
}

// pthread_sigmask 当前线程，屏蔽信号 new_mask, 旧的会存在 saved_sigmask 中。
// 1. 所有的信号将阻塞（除了几个无法阻塞的信号）。
// 2. 等待管道里的 1 个数据，大于即可解锁。
static void uv__signal_block_and_lock(sigset_t* saved_sigmask) {
  sigset_t new_mask;

  if (sigfillset(&new_mask))
    abort();

  /* to shut up valgrind */
  sigemptyset(saved_sigmask);
  if (pthread_sigmask(SIG_SETMASK, &new_mask, saved_sigmask))
    abort();

  if (uv__signal_lock())
    abort();
}

// pthread_sigmask 当前线程，屏蔽信号 new_mask
static void uv__signal_unlock_and_unblock(sigset_t* saved_sigmask) {
  if (uv__signal_unlock())
    abort();

  if (pthread_sigmask(SIG_SETMASK, saved_sigmask, NULL))
    abort();
}

// 找到信号对应的第一个handle。
static uv_signal_t* uv__signal_first_handle(int signum) {
  /* This function must be called with the signal lock held. */
  uv_signal_t lookup;
  uv_signal_t* handle;

  lookup.signum = signum;
  lookup.flags = 0;
  lookup.loop = NULL;

  // 从红黑树中查询到 handle，确定这个 handle 
  handle = RB_NFIND(uv__signal_tree_s, &uv__signal_tree, &lookup);

  if (handle != NULL && handle->signum == signum)
    return handle;

  return NULL;
}

// 所有的信号额处理函数将都会注册为这个函数。再由这个函数分发信号给具体的 uv_signal_t handle 对应的回调函数 
// sgaction 中的 handle void (*) (int) sa_handler：处理函数指针，相当于signal函数的func参数。
static void uv__signal_handler(int signum) {
  uv__signal_msg_t msg;
  uv_signal_t* handle;
  int saved_errno;

  saved_errno = errno;
  memset(&msg, 0, sizeof msg);

  if (uv__signal_lock()) {
    errno = saved_errno;
    return;
  }

  // 找到信号的的第一个handle。其实是对应的 handle，信号和handle是1对1的关系
  for (handle = uv__signal_first_handle(signum);
       handle != NULL && handle->signum == signum; // 如果 handle 的 sigum 则继续查，找到最后插入的 1 个节点 handle 作为信号处理函数
       handle = RB_NEXT(uv__signal_tree_s, &uv__signal_tree, handle)) {
    int r;

    // 找到了 handle
    msg.signum = signum;
    msg.handle = handle;

    // 写入消息体到 signal_pipefd。这样在 epoll 时，能够找到一个线程处理这个 msg
    /* write() should be atomic for small data chunks, so the entire message
     * should be written at once. In theory the pipe could become full, in
     * which case the user is out of luck.
     */
    do {
      r = write(handle->loop->signal_pipefd[1], &msg, sizeof msg);
    } while (r == -1 && errno == EINTR);

    assert(r == sizeof msg ||
           (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)));

    // 计算已经执行成果的信号数量
    if (r != -1)
      handle->caught_signals++;
  }

  // 解锁当前的信号
  uv__signal_unlock();
  errno = saved_errno;
}


static int uv__signal_register_handler(int signum, int oneshot) {
  // void (*) (int) sa_handler：处理函数指针，相当于signal函数的func参数。
  // sigset_t sa_mask： 指定一个。信号集，在调用sa_handler所指向的信号处理函数之前，该信号集将被加入到进程的信号屏蔽字中。信号屏蔽字是指当前被阻塞的一组信号，它们不能被当前进程接收到
  // int sa_flags：信号处理修改器;
  /* When this function is called, the signal lock must be held. */
  struct sigaction sa;

  /* XXX use a separate signal stack? */
  memset(&sa, 0, sizeof(sa));
  if (sigfillset(&sa.sa_mask)) // 当前信号被函数处理 uv__signal_handler 时，所有的信号直接屏蔽掉。防止当前信号处理函数被其他信号中断
    abort();
  sa.sa_handler = uv__signal_handler; // 给当前这个 signum 设置上 uv__signal_handler。每个 signum 对应的都是 uv__signal_handler
  sa.sa_flags = SA_RESTART;
  if (oneshot) // 如果注册的是 oneshot(一次执行后，自动恢复默认) 的话
    sa.sa_flags |= SA_RESETHAND;

  /* XXX save old action so we can restore it later on? */
  if (sigaction(signum, &sa, NULL))
    return UV__ERR(errno);

  return 0;
}


static void uv__signal_unregister_handler(int signum) {
  /* When this function is called, the signal lock must be held. */
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;

  /* sigaction can only fail with EINVAL or EFAULT; an attempt to deregister a
   * signal implies that it was successfully registered earlier, so EINVAL
   * should never happen.
   */
  if (sigaction(signum, &sa, NULL))
    abort();
}

// 每个 signal 类型的 handle，初始化时执行 1 次。
// 1）申请一个管道，libuv fork 出来的进程和 libuv 进程通信。
// 然后往 libuv 的 io 观察者队列注册一个观察者，libuv 在 poll io 阶段会把观察者加到
// epoll 中。io 观察者里保存了管道读端的文件描述符 loop->signal_pipefd[0]和回调函
// 数 uv__signal_event。uv__signal_event 是任意信号触发时的回调，他会继续根据触发的信号进行逻辑分发。
// 2）初始化信号信号 handle 的字段。
static int uv__signal_loop_once_init(uv_loop_t* loop) {
  int err;

  /* Return if already initialized. */
  if (loop->signal_pipefd[0] != -1)
    return 0;

  // 创建 loop->signal_pipefd 创建 pipe。
  err = uv__make_pipe(loop->signal_pipefd, UV_NONBLOCK_PIPE);
  if (err)
    return err;

  // 初始化 loop->signal_io_watcher。signal_io_watcher 是 uv__io_s 类型的。
  // io 阶段 不用同于 idler 这种。io 阶段的 handle 有不同的子类型。例如：信号。
  uv__io_init(&loop->signal_io_watcher,
              uv__signal_event,        // 当有消息发送给 loop->signal_pipefd[1]， 执行 uv__signal_event
              loop->signal_pipefd[0]); // 这里父进程为什么使用 loop->signal_pipefd[0], 而不是 1。因为这里监听的读事件
  // signal_io_watcher 放置到 loop 中运行。
  // POLLIN 有数据可读。POLLOUT 写数据不会导致阻塞。POLLER 指定的文件描述符发生错误。
  uv__io_start(loop, &loop->signal_io_watcher, POLLIN);

  return 0;
}


int uv__signal_loop_fork(uv_loop_t* loop) {
  uv__io_stop(loop, &loop->signal_io_watcher, POLLIN);
  uv__close(loop->signal_pipefd[0]);
  uv__close(loop->signal_pipefd[1]);
  loop->signal_pipefd[0] = -1;
  loop->signal_pipefd[1] = -1;
  return uv__signal_loop_once_init(loop);
}

// loop 结束时清理信号
void uv__signal_loop_cleanup(uv_loop_t* loop) {
  QUEUE* q;

  /* Stop all the signal watchers that are still attached to this loop. This
   * ensures that the (shared) signal tree doesn't contain any invalid entries
   * entries, and that signal handlers are removed when appropriate.
   * It's safe to use QUEUE_FOREACH here because the handles and the handle
   * queue are not modified by uv__signal_stop().
   */
  QUEUE_FOREACH(q, &loop->handle_queue) {
    uv_handle_t* handle = QUEUE_DATA(q, uv_handle_t, handle_queue);

    if (handle->type == UV_SIGNAL)
      uv__signal_stop((uv_signal_t*) handle);
  }

  if (loop->signal_pipefd[0] != -1) {
    uv__close(loop->signal_pipefd[0]);
    loop->signal_pipefd[0] = -1;
  }

  if (loop->signal_pipefd[1] != -1) {
    uv__close(loop->signal_pipefd[1]);
    loop->signal_pipefd[1] = -1;
  }
}

// loop 启动时内部调用 1 次。初始化 uv_signal_t 类型的 handle。
int uv_signal_init(uv_loop_t* loop, uv_signal_t* handle) {
  int err;

  // 注意这里只会执行 1 次。信号 handle 共享 loop->signal_pipefd
  // 初始化 loop 底层的 signal_io_watcher, 创建的 fd 存在 loop->signal_pipefd
  err = uv__signal_loop_once_init(loop);
  if (err)
    return err;

  // uv__handle_init 是所有函数的公共初始化
  uv__handle_init(loop, (uv_handle_t*) handle, UV_SIGNAL);
  handle->signum = 0;
  handle->caught_signals = 0;
  handle->dispatched_signals = 0;

  return 0;
}


void uv__signal_close(uv_signal_t* handle) {
  uv__signal_stop(handle);
}

// 启动 uv_signal_t* handle，signal_cb 执行一次后恢复。
int uv_signal_start(uv_signal_t* handle, uv_signal_cb signal_cb, int signum) {
  return uv__signal_start(handle, signal_cb, signum, 0);
}

// 其中只执行一次的 uv_signal_t* handle
int uv_signal_start_oneshot(uv_signal_t* handle,
                            uv_signal_cb signal_cb,
                            int signum) {
  return uv__signal_start(handle, signal_cb, signum, 1);
}

// 启动 uv_signal_t 类型的 handle。调用 uv_spawn 时，在父进程中运行。
// 1 handle->signum != 0 意味着这个 handle 是已经 active 状态的。
// 2 active 状态的 handle，调用这个函数只会替换 signal_cb。
static int uv__signal_start(uv_signal_t* handle,
                            uv_signal_cb signal_cb, // 信号回调函数
                            int signum,             // 对应的信号编号
                            int oneshot) {          // 是否是 oneshot 的
  sigset_t saved_sigmask;
  int err;
  uv_signal_t* first_handle;

  // 不能是1个正在关闭的 uv_signal_t handle
  assert(!uv__is_closing(handle));

  /* If the user supplies signum == 0, then return an error already. If the
   * signum is otherwise invalid then uv__signal_register will find out
   * eventually.
   */
  if (signum == 0)
    return UV_EINVAL;

  // handle->signum 和传入 signum 一致，表示需要替换新的 signal_cb 即可。
  /* Short circuit: if the signal watcher is already watching {signum} don't
   * go through the process of deregistering and registering the handler.
   * Additionally, this avoids pending signals getting lost in the small
   * time frame that handle->signum == 0.
   */
  if (signum == handle->signum) {
    handle->signal_cb = signal_cb;
    return 0;
  }

  // 如果 handle->signum 不是传入的 signum 且 handle->signum 不是 0 。这个 handle 属于已激活的。先将 handlestop 掉。
  /* If the signal handler was already active, stop it first. */
  if (handle->signum != 0) {
    uv__signal_stop(handle);
  }

  // 屏蔽所有信号，并且获取到锁
  uv__signal_block_and_lock(&saved_sigmask);

  // 如果没有其他的 signal handle 监听这个 signum.
  /* NO JAMLEE
  1 之前没有注册过该信号的处理函数则直接设置
  2 之前设置过，但是是 one shot，但是现在需要设置的规则不是 one shot，需要修改。否则第二次不会不会触发。因为一个信号只能对应一
  个信号处理函数，所以，以规则宽的为准备，在回调里再根据 flags 判断是不是真的需要执行
  3 如果注册过信号和处理函数，则直接插入红黑树就行。
  */
  /* If at this point there are no active signal watchers for this signum (in
   * any of the loops), it's time to try and register a handler for it here.
   * Also in case there's only one-shot handlers and a regular handler comes in.
   */
  first_handle = uv__signal_first_handle(signum);
  if (first_handle == NULL ||
      (!oneshot && (first_handle->flags & UV_SIGNAL_ONE_SHOT))) { // 如果没有注册过这个信号; 如果当前不是 oneshot 而查到的handle是oneshot，以当前的为准
    err = uv__signal_register_handler(signum, oneshot); // 注册信号
    if (err) {
      /* Registering the signal handler failed. Must be an invalid signal. */
      uv__signal_unlock_and_unblock(&saved_sigmask);
      return err;
    }
  }

  handle->signum = signum;
  if (oneshot)
    handle->flags |= UV_SIGNAL_ONE_SHOT; // 是只执行 1 次，这个 flag 打开

  // handle 虽然在信号中已经注册，但是是更新的 uv__signal_handler 函数。这里插入红黑树，以便 uv__signal_handler 路由
  RB_INSERT(uv__signal_tree_s, &uv__signal_tree, handle);
  // 解锁 signal 管理模块
  uv__signal_unlock_and_unblock(&saved_sigmask);

  handle->signal_cb = signal_cb; // 设置信号的回调为 signal_cb
  uv__handle_start(handle); // 设置为激活状态，当 loop->signal_pipefd[0] 有数据可读时，那么 io_watcher 会往其中写入数据的。

  return 0;
}

// 任意信号事件分发函数。信号对应着int值。这里是 loop->signal_pipefd[0] 中有数据可读时触发。
// 1	SIGHUP	挂起	 
// 2	SIGINT	中断	 
// 3	SIGQUIT	退出	 
// 4	SIGILL	非法指令	 
// 5	SIGTRAP	断点或陷阱指令	 
// 6	SIGABRT	abort发出的信号	 
// 7	SIGBUS	非法内存访问	 
// 8	SIGFPE	浮点异常	 
// 9	SIGKILL	kill信号	不能被忽略、处理和阻塞
// 10	SIGUSR1	用户信号1	 
// 11	SIGSEGV	无效内存访问	 
// 12	SIGUSR2	用户信号2	 
// 13	SIGPIPE	管道破损，没有读端的管道写数据	 
// 14	SIGALRM	alarm发出的信号	 
// 15	SIGTERM	终止信号	 
// 16	SIGSTKFLT	栈溢出	 
// 17	SIGCHLD	子进程退出	默认忽略
// 18	SIGCONT	进程继续	 
// 19	SIGSTOP	进程停止	不能被忽略、处理和阻塞
// 20	SIGTSTP	进程停止	 
// 21	SIGTTIN	进程停止，后台进程从终端读数据时	 
// 22	SIGTTOU	进程停止，后台进程想终端写数据时	 
// 23	SIGURG	I/O有紧急数据到达当前进程	默认忽略
// 24	SIGXCPU	进程的CPU时间片到期	 
// 25	SIGXFSZ	文件大小的超出上限	 
// 26	SIGVTALRM	虚拟时钟超时	 
// 27	SIGPROF	profile时钟超时	 
// 28	SIGWINCH	窗口大小改变	默认忽略
// 29	SIGIO	I/O相关	 
// 30	SIGPWR	关机	默认忽略
// 31	SIGSYS	系统调用异常
static void uv__signal_event(uv_loop_t* loop,
                             uv__io_t* w,
                             unsigned int events) {
  uv__signal_msg_t* msg; // 信号消息结构体
  uv_signal_t* handle; // 信号 handle
  char buf[sizeof(uv__signal_msg_t) * 32];
  size_t bytes, end, i;
  int r;

  bytes = 0;
  end = 0;

  do {
    r = read(loop->signal_pipefd[0], buf + bytes, sizeof(buf) - bytes);

    if (r == -1 && errno == EINTR)
      continue;

    if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      /* If there are bytes in the buffer already (which really is extremely
       * unlikely if possible at all) we can't exit the function here. We'll
       * spin until more bytes are read instead.
       */
      if (bytes > 0)
        continue;

      /* Otherwise, there was nothing there. */
      return;
    }

    /* Other errors really should never happen. */
    if (r == -1)
      abort();

    bytes += r;

    /* `end` is rounded down to a multiple of sizeof(uv__signal_msg_t). */
    end = (bytes / sizeof(uv__signal_msg_t)) * sizeof(uv__signal_msg_t);
    // 从管道里面读取的是一个完整的 uv__signal_msg_t 结构体。
    for (i = 0; i < end; i += sizeof(uv__signal_msg_t)) {
      msg = (uv__signal_msg_t*) (buf + i);
      handle = msg->handle;

      if (msg->signum == handle->signum) {
        assert(!(handle->flags & UV_HANDLE_CLOSING));
        handle->signal_cb(handle, handle->signum);
      }

      handle->dispatched_signals++;

      if (handle->flags & UV_SIGNAL_ONE_SHOT)
        uv__signal_stop(handle);
    }

    bytes -= end;

    /* If there are any "partial" messages left, move them to the start of the
     * the buffer, and spin. This should not happen.
     */
    if (bytes) {
      memmove(buf, buf + end, bytes);
      continue;
    }
  } while (end == sizeof buf);
}


static int uv__signal_compare(uv_signal_t* w1, uv_signal_t* w2) {
  int f1;
  int f2;
  /* Compare signums first so all watchers with the same signnum end up
   * adjacent.
   */
  if (w1->signum < w2->signum) return -1;
  if (w1->signum > w2->signum) return 1;

  /* Handlers without UV_SIGNAL_ONE_SHOT set will come first, so if the first
   * handler returned is a one-shot handler, the rest will be too.
   */
  f1 = w1->flags & UV_SIGNAL_ONE_SHOT;
  f2 = w2->flags & UV_SIGNAL_ONE_SHOT;
  if (f1 < f2) return -1;
  if (f1 > f2) return 1;

  /* Sort by loop pointer, so we can easily look up the first item after
   * { .signum = x, .loop = NULL }.
   */
  if (w1->loop < w2->loop) return -1;
  if (w1->loop > w2->loop) return 1;

  if (w1 < w2) return -1;
  if (w1 > w2) return 1;

  return 0;
}


int uv_signal_stop(uv_signal_t* handle) {
  assert(!uv__is_closing(handle));
  uv__signal_stop(handle);
  return 0;
}

// stop 后的 hanlde 还可以 start，close 的会完全消失。
static void uv__signal_stop(uv_signal_t* handle) {
  uv_signal_t* removed_handle;
  sigset_t saved_sigmask;
  uv_signal_t* first_handle;
  int rem_oneshot;
  int first_oneshot;
  int ret;

  // 如果这个 handle 没有启动过，不用 stop。
  /* If the watcher wasn't started, this is a no-op. */
  if (handle->signum == 0)
    return;

  // 屏蔽所有信号然后获取1个锁。
  uv__signal_block_and_lock(&saved_sigmask);

  // 首先从红黑树种移除这个 handle
  removed_handle = RB_REMOVE(uv__signal_tree_s, &uv__signal_tree, handle);
  assert(removed_handle == handle);
  (void) removed_handle;

  // 检查是否有其他激活的 signal watchers 监听这个 signum。如果没有取消注册 signal handle
  /* Check if there are other active signal watchers observing this signal. If
   * not, unregister the signal handler.
   */
  first_handle = uv__signal_first_handle(handle->signum);
  if (first_handle == NULL) {
    uv__signal_unregister_handler(handle->signum); // 如果这个信号没有任何 uv_signal_t 了
  } else { // 否则，注册成 onshot 形式的 handle，执行完毕后系统会自动恢复系统默认的 handle
    rem_oneshot = handle->flags & UV_SIGNAL_ONE_SHOT;
    first_oneshot = first_handle->flags & UV_SIGNAL_ONE_SHOT;
    if (first_oneshot && !rem_oneshot) {
      ret = uv__signal_register_handler(handle->signum, 1); 
      assert(ret == 0);
      (void)ret;
    }
  }

  uv__signal_unlock_and_unblock(&saved_sigmask);

  handle->signum = 0;
  uv__handle_stop(handle);
}
