/* Copyright libuv contributors. All rights reserved.
 *
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
#include <errno.h>
#include <sys/epoll.h>

// linux 下使用 epoll。
// 1. epoll_create 返回 1 个 efd
// 2. epoll_ctl 函数把 listen 的 fd 加入到 efd 中。
// 3. epoll_wait 等待 efd 上的事件。
int uv__epoll_init(uv_loop_t* loop) {
  int fd;
  fd = epoll_create1(O_CLOEXEC);

  /* epoll_create1() can fail either because it's not implemented (old kernel)
   * or because it doesn't understand the O_CLOEXEC flag.
   */
  if (fd == -1 && (errno == ENOSYS || errno == EINVAL)) {
    fd = epoll_create(256);

    if (fd != -1)
      uv__cloexec(fd, 1);
  }
  // loop 的 backend_fd 是 epoll 的 efd。
  loop->backend_fd = fd;
  if (fd == -1)
    return UV__ERR(errno);

  return 0;
}


void uv__platform_invalidate_fd(uv_loop_t* loop, int fd) {
  struct epoll_event* events;
  struct epoll_event dummy;
  uintptr_t i;
  uintptr_t nfds;

  assert(loop->watchers != NULL);
  assert(fd >= 0);

  events = (struct epoll_event*) loop->watchers[loop->nwatchers];
  nfds = (uintptr_t) loop->watchers[loop->nwatchers + 1];
  if (events != NULL)
    /* Invalidate events with same file descriptor */
    for (i = 0; i < nfds; i++)
      if (events[i].data.fd == fd)
        events[i].data.fd = -1;

  /* Remove the file descriptor from the epoll.
   * This avoids a problem where the same file description remains open
   * in another process, causing repeated junk epoll events.
   *
   * We pass in a dummy epoll_event, to work around a bug in old kernels.
   */
  if (loop->backend_fd >= 0) {
    /* Work around a bug in kernels 3.10 to 3.19 where passing a struct that
     * has the EPOLLWAKEUP flag set generates spurious audit syslog warnings.
     */
    memset(&dummy, 0, sizeof(dummy));
    epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, &dummy);
  }
}


int uv__io_check_fd(uv_loop_t* loop, int fd) {
  struct epoll_event e;
  int rc;

  memset(&e, 0, sizeof(e));
  e.events = POLLIN;
  e.data.fd = -1;

  rc = 0;
  if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &e))
    if (errno != EEXIST)
      rc = UV__ERR(errno);

  if (rc == 0)
    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, &e))
      abort();

  return rc;
}


void uv__io_poll(uv_loop_t* loop, int timeout) {
  /* A bug in kernels < 2.6.37 makes timeouts larger than ~30 minutes
   * effectively infinite on 32 bits architectures.  To avoid blocking
   * indefinitely, we cap the timeout and poll again if necessary.
   *
   * Note that "30 minutes" is a simplification because it depends on
   * the value of CONFIG_HZ.  The magic constant assumes CONFIG_HZ=1200,
   * that being the largest value I have seen in the wild (and only once.)
   */
  static const int max_safe_timeout = 1789569;
  static int no_epoll_pwait_cached;
  static int no_epoll_wait_cached;
  int no_epoll_pwait;
  int no_epoll_wait;
  struct epoll_event events[1024]; // 最大可以处理的事件数
  struct epoll_event* pe;
  struct epoll_event e;
  int real_timeout;
  QUEUE* q;
  uv__io_t* w;
  sigset_t sigset;   // 信号屏蔽字
  uint64_t sigmask;
  uint64_t base;
  int have_signals;
  int nevents; // 循环中事件计数器
  int count;
  int nfds;
  int fd;
  int op;
  int i;
  int user_timeout;
  int reset_timeout;

  // 如果 fd 计数为 0。退出这个函数
  if (loop->nfds == 0) {
    assert(QUEUE_EMPTY(&loop->watcher_queue));
    return;
  }

  memset(&e, 0, sizeof(e));

  // 循环遍历 watcher_queue 取出的结构体为 io watcher。添加到 epoll 监听队列中
  while (!QUEUE_EMPTY(&loop->watcher_queue)) {
    q = QUEUE_HEAD(&loop->watcher_queue);
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);

    // 取出 q 对应的io watcher 结构体。w
    w = QUEUE_DATA(q, uv__io_t, watcher_queue);
    assert(w->pevents != 0);
    assert(w->fd >= 0);
    assert(w->fd < (int) loop->nwatchers);

    e.events = w->pevents; // watcher 的 pending events 赋值给 e.events
    e.data.fd = w->fd;     // 

    if (w->events == 0)
      op = EPOLL_CTL_ADD;
    else
      op = EPOLL_CTL_MOD;

    // w->fd 和 w->events 赋值给 epoll_event。
    // 参数1：
    // EPOLL_CTL_ADD：注册新的fd到epfd中；
    // EPOLL_CTL_MOD：修改已经注册的fd的监听事件；
    // EPOLL_CTL_DEL：从epfd中删除一个fd；
    //
    // 参数2：
    // 需要添加的 w->fd
    //
    // 参数3：
    // epoll_event
    // typedef union epoll_data {
    //   void *ptr;
    //   int fd;
    //   __uint32_t u32;
    //   __uint64_t u64;
    // } epoll_data_t;
    
    // struct epoll_event {
    //   __uint32_t events; /* Epoll events */
    //   epoll_data_t data; /* User data variable */
    // }

    /* XXX Future optimization: do EPOLL_CTL_MOD lazily if we stop watching
     * events, skip the syscall and squelch the events after epoll_wait().
     */
    if (epoll_ctl(loop->backend_fd, op, w->fd, &e)) {
      if (errno != EEXIST)
        abort();

      assert(op == EPOLL_CTL_ADD);

      /* We've reactivated a file descriptor that's been watched before. */
      if (epoll_ctl(loop->backend_fd, EPOLL_CTL_MOD, w->fd, &e))
        abort();
    }

    w->events = w->pevents; // 本次已经加入过的 epoll 中的 pevents 存储到 events。
  }

  // 设置信号处理掩码
  // 1. epoll_pwait相比epoll_wait多了屏蔽指定信号的功能，即通过指定sigmask可以在 epoll_pwait函数中修改信号屏蔽设置，epoll_pwait返回后恢复原始信号屏蔽设置
  // 2. 在epoll_wait阻塞期间，signal信号会打断该阻塞，使其返回-1并将errno设置成EINTR。
  sigmask = 0;
  if (loop->flags & UV_LOOP_BLOCK_SIGPROF) { // UV_LOOP_BLOCK_SIGPROF 是 loop 的 flag。setitimer 函数会产生这个信号给进程。
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGPROF);
    sigmask |= 1 << (SIGPROF - 1); // SIGPROF 是 27 号。https://man7.org/linux/man-pages/man7/signal.7.html
    // sigmask 此时是 SIGPROF 了
  }

  assert(timeout >= -1);
  base = loop->time; // loop 的时间戳
  count = 48; /* Benchmarks suggest this gives the best throughput. */
  real_timeout = timeout;

  if (uv__get_internal_fields(loop)->flags & UV_METRICS_IDLE_TIME) {
    reset_timeout = 1;
    user_timeout = timeout;
    timeout = 0;
  } else {
    reset_timeout = 0;
    user_timeout = 0;
  }

  /* You could argue there is a dependency between these two but
   * ultimately we don't care about their ordering with respect
   * to one another. Worst case, we make a few system calls that
   * could have been avoided because another thread already knows
   * they fail with ENOSYS. Hardly the end of the world.
   */
  no_epoll_pwait = uv__load_relaxed(&no_epoll_pwait_cached); // __atomic_load_n gnu 原子操作
  no_epoll_wait = uv__load_relaxed(&no_epoll_wait_cached);

  // 循环
  for (;;) {
    /* Only need to set the provider_entry_time if timeout != 0. The function
     * will return early if the loop isn't configured with UV_METRICS_IDLE_TIME.
     */
    if (timeout != 0) // 如果 timeout 不等于 0。才会设置 entry time
      uv__metrics_set_provider_entry_time(loop);

    /* See the comment for max_safe_timeout for an explanation of why
     * this is necessary.  Executive summary: kernel bug workaround.
     */
    if (sizeof(int32_t) == sizeof(long) && timeout >= max_safe_timeout)
      timeout = max_safe_timeout;

    // 如果loop设置了 sigmask 且 当前是 no_epoll_pwait 。当前线程阻塞信号，也就是说用的是 epoll 而不是 epoll_wait
    if (sigmask != 0 && no_epoll_pwait != 0)
      if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
        abort();

    if (no_epoll_wait != 0 || (sigmask != 0 && no_epoll_pwait == 0)) {
      nfds = epoll_pwait(loop->backend_fd,
                         events,  // events 用来接收发生的事件，是 1 个数组
                         ARRAY_SIZE(events),
                         timeout,
                         &sigset);
      if (nfds == -1 && errno == ENOSYS) {
        uv__store_relaxed(&no_epoll_pwait_cached, 1);
        no_epoll_pwait = 1; // 当前出错了切换到 epoll_wait。errno.h中定义#define ENOSYS 38 /* Function not implemented */
      }
    } else {
      nfds = epoll_wait(loop->backend_fd,
                        events, // events 用来接收发生的事件，是 1 个数组
                        ARRAY_SIZE(events),
                        timeout);
      if (nfds == -1 && errno == ENOSYS) {
        uv__store_relaxed(&no_epoll_wait_cached, 1);
        no_epoll_wait = 1;  // 当前出错了切换到 epoll_pwait。//errno.h中定义#define ENOSYS 38 /* Function not implemented */
      }
    }

    // 取消线程的信号阻塞
    if (sigmask != 0 && no_epoll_pwait != 0)
      if (pthread_sigmask(SIG_UNBLOCK, &sigset, NULL))
        abort();

    /* Update loop->time unconditionally. It's tempting to skip the update when
     * timeout == 0 (i.e. non-blocking poll) but there is no guarantee that the
     * operating system didn't reschedule our process while in the syscall.
     */
    SAVE_ERRNO(uv__update_time(loop));

    // 如果 epoll 发现没有事件产生
    if (nfds == 0) {
      assert(timeout != -1);

      if (reset_timeout != 0) {
        timeout = user_timeout;
        reset_timeout = 0;
      }

      if (timeout == -1)
        continue;

      if (timeout == 0)
        return;

      /* We may have been inside the system call for longer than |timeout|
       * milliseconds so we need to update the timestamp to avoid drift.
       */
      goto update_timeout;
    }

    // 如果 epoll 出错
    if (nfds == -1) {
      if (errno == ENOSYS) {
        /* epoll_wait() or epoll_pwait() failed, try the other system call. */
        assert(no_epoll_wait == 0 || no_epoll_pwait == 0);
        continue;
      }

      if (errno != EINTR)
        abort();

      if (reset_timeout != 0) {
        timeout = user_timeout;
        reset_timeout = 0;
      }

      if (timeout == -1)
        continue;

      if (timeout == 0)
        return;

      /* Interrupted by a signal. Update timeout and poll again. */
      goto update_timeout;
    }

    have_signals = 0;
    nevents = 0;

    {
      /* Squelch a -Waddress-of-packed-member warning with gcc >= 9. */
      union {
        struct epoll_event* events;
        uv__io_t* watchers;
      } x;

      x.events = events;
      assert(loop->watchers != NULL);
      loop->watchers[loop->nwatchers] = x.watchers;
      loop->watchers[loop->nwatchers + 1] = (void*) (uintptr_t) nfds;
    }

    // 如果 epoll 取到 事件了
    for (i = 0; i < nfds; i++) {
      pe = events + i; // 取 events 的中的 pe
      fd = pe->data.fd; // 取到 pe 对应 fd

      /* Skip invalidated events, see uv__platform_invalidate_fd */
      if (fd == -1)
        continue;

      assert(fd >= 0);
      assert((unsigned) fd < loop->nwatchers);

      w = loop->watchers[fd]; // 取出对应的 io watcher

      // 如果是 io watcher 是已经停止的
      if (w == NULL) {
        /* File descriptor that we've stopped watching, disarm it.
         *
         * Ignore all errors because we may be racing with another thread
         * when the file descriptor is closed.
         */
        epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, pe);
        continue;
      }

      /* Give users only events they're interested in. Prevents spurious
       * callbacks when previous callback invocation in this loop has stopped
       * the current watcher. Also, filters out events that users has not
       * requested us to watch.
       */
      pe->events &= w->pevents | POLLERR | POLLHUP;

      /* Work around an epoll quirk where it sometimes reports just the
       * EPOLLERR or EPOLLHUP event.  In order to force the event loop to
       * move forward, we merge in the read/write events that the watcher
       * is interested in; uv__read() and uv__write() will then deal with
       * the error or hangup in the usual fashion.
       *
       * Note to self: happens when epoll reports EPOLLIN|EPOLLHUP, the user
       * reads the available data, calls uv_read_stop(), then sometime later
       * calls uv_read_start() again.  By then, libuv has forgotten about the
       * hangup and the kernel won't report EPOLLIN again because there's
       * nothing left to read.  If anything, libuv is to blame here.  The
       * current hack is just a quick bandaid; to properly fix it, libuv
       * needs to remember the error/hangup event.  We should get that for
       * free when we switch over to edge-triggered I/O.
       */
      if (pe->events == POLLERR || pe->events == POLLHUP)
        pe->events |=
          w->pevents & (POLLIN | POLLOUT | UV__POLLRDHUP | UV__POLLPRI);

      if (pe->events != 0) {
        /* Run signal watchers last.  This also affects child process watchers
         * because those are implemented in terms of signal watchers.
         */
        if (w == &loop->signal_io_watcher) { // 如果 w 是 signal_io_watcher。have_signals 赋值为 1
          have_signals = 1;
        } else {
          uv__metrics_update_idle_time(loop);
          w->cb(loop, w, pe->events);
        }

        nevents++;
      }
    }

    if (reset_timeout != 0) {
      timeout = user_timeout;
      reset_timeout = 0;
    }

    // 如果接收到信号。对应的是
    if (have_signals != 0) {
      uv__metrics_update_idle_time(loop);
      loop->signal_io_watcher.cb(loop, &loop->signal_io_watcher, POLLIN);
    }

    loop->watchers[loop->nwatchers] = NULL;
    loop->watchers[loop->nwatchers + 1] = NULL;

    if (have_signals != 0)
      return;  /* Event loop should cycle now so don't poll again. */

    if (nevents != 0) {
      if (nfds == ARRAY_SIZE(events) && --count != 0) {
        /* Poll for more events but don't block this time. */
        timeout = 0;
        continue;
      }
      return;
    }

    if (timeout == 0)
      return;

    if (timeout == -1)
      continue;

update_timeout:
    assert(timeout > 0);

    real_timeout -= (loop->time - base);
    if (real_timeout <= 0)
      return;

    timeout = real_timeout;
  }
}

