/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#if defined(__APPLE__) && !TARGET_OS_IPHONE
# include <crt_externs.h>
# define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

#if defined(__linux__) || defined(__GLIBC__)
# include <grp.h>
#endif

#if defined(__MVS__)
# include "zos-base.h"
#endif


static void uv__chld(uv_signal_t* handle, int signum) {
  uv_process_t* process;
  uv_loop_t* loop;
  int exit_status;
  int term_signal;
  int status;
  pid_t pid;
  QUEUE pending;
  QUEUE* q;
  QUEUE* h;

  assert(signum == SIGCHLD);

  QUEUE_INIT(&pending);
  loop = handle->loop;

  h = &loop->process_handles;
  q = QUEUE_HEAD(h);
  while (q != h) {
    process = QUEUE_DATA(q, uv_process_t, queue);
    q = QUEUE_NEXT(q);

    do
      pid = waitpid(process->pid, &status, WNOHANG);
    while (pid == -1 && errno == EINTR);

    if (pid == 0)
      continue;

    if (pid == -1) {
      if (errno != ECHILD)
        abort();
      continue;
    }

    process->status = status;
    QUEUE_REMOVE(&process->queue);
    QUEUE_INSERT_TAIL(&pending, &process->queue);
  }

  h = &pending;
  q = QUEUE_HEAD(h);
  while (q != h) {
    process = QUEUE_DATA(q, uv_process_t, queue);
    q = QUEUE_NEXT(q);

    QUEUE_REMOVE(&process->queue);
    QUEUE_INIT(&process->queue);
    uv__handle_stop(process);

    if (process->exit_cb == NULL)
      continue;

    exit_status = 0;
    if (WIFEXITED(process->status))
      exit_status = WEXITSTATUS(process->status);

    term_signal = 0;
    if (WIFSIGNALED(process->status))
      term_signal = WTERMSIG(process->status);

    process->exit_cb(process, exit_status, term_signal);
  }
  assert(QUEUE_EMPTY(&pending));
}

// JAMLEE: 初始化 container。传入 1 个 pipe
/*
 * Used for initializing stdio streams like options.stdin_stream. Returns
 * zero on success. See also the cleanup section in uv_spawn().
 */
static int uv__process_init_stdio(uv_stdio_container_t* container, int fds[2]) {
  int mask;
  int fd;

  mask = UV_IGNORE | UV_CREATE_PIPE | UV_INHERIT_FD | UV_INHERIT_STREAM;

  switch (container->flags & mask) {
  case UV_IGNORE:
    return 0;

  case UV_CREATE_PIPE: // JAMLEE: 创建 pipe
    assert(container->data.stream != NULL);
    if (container->data.stream->type != UV_NAMED_PIPE)
      return UV_EINVAL;
    else
      return uv_socketpair(SOCK_STREAM, 0, fds, 0, 0);

  case UV_INHERIT_FD:
  case UV_INHERIT_STREAM:
    if (container->flags & UV_INHERIT_FD)
      fd = container->data.fd;
    else
      fd = uv__stream_fd(container->data.stream);

    if (fd == -1)
      return UV_EINVAL;

    fds[1] = fd;
    return 0;

  default:
    assert(0 && "Unexpected flags");
    return UV_EINVAL;
  }
}


static int uv__process_open_stream(uv_stdio_container_t* container,
                                   int pipefds[2]) {
  int flags;
  int err;

  if (!(container->flags & UV_CREATE_PIPE) || pipefds[0] < 0)
    return 0;

  err = uv__close(pipefds[1]);
  if (err != 0)
    abort();

  pipefds[1] = -1;
  uv__nonblock(pipefds[0], 1);

  flags = 0;
  if (container->flags & UV_WRITABLE_PIPE)
    flags |= UV_HANDLE_READABLE;
  if (container->flags & UV_READABLE_PIPE)
    flags |= UV_HANDLE_WRITABLE;

  return uv__stream_open(container->data.stream, pipefds[0], flags);
}


static void uv__process_close_stream(uv_stdio_container_t* container) {
  if (!(container->flags & UV_CREATE_PIPE)) return;
  uv__stream_close(container->data.stream);
}


static void uv__write_int(int fd, int val) {
  ssize_t n;

  do
    n = write(fd, &val, sizeof(val));
  while (n == -1 && errno == EINTR);

  if (n == -1 && errno == EPIPE)
    return; /* parent process has quit */

  assert(n == sizeof(val));
}


static void uv__write_errno(int error_fd) {
  uv__write_int(error_fd, UV__ERR(errno));
  _exit(127);
}


// JAMLEE: 在子进程中运行。当fork成功时，这个函数第一个运行。
#if !(defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH))
/* execvp is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED, so must be
 * avoided. Since this isn't called on those targets, the function
 * doesn't even need to be defined for them.
 */
static void uv__process_child_init(const uv_process_options_t* options,
                                   int stdio_count,
                                   int (*pipes)[2],
                                   int error_fd) {
  sigset_t signewset;
  int close_fd;
  int use_fd;
  int fd;
  int n;

  /* Reset signal disposition first. Use a hard-coded limit because NSIG is not
   * fixed on Linux: it's either 32, 34 or 64, depending on whether RT signals
   * are enabled. We are not allowed to touch RT signal handlers, glibc uses
   * them internally.
   */
  for (n = 1; n < 32; n += 1) {
    if (n == SIGKILL || n == SIGSTOP)
      continue;  /* Can't be changed. */

#if defined(__HAIKU__)
    if (n == SIGKILLTHR)
      continue;  /* Can't be changed. */
#endif

    if (SIG_ERR != signal(n, SIG_DFL))
      continue;

    uv__write_errno(error_fd);
  }

  if (options->flags & UV_PROCESS_DETACHED)
    setsid();

  /* First duplicate low numbered fds, since it's not safe to duplicate them,
   * they could get replaced. Example: swapping stdout and stderr; without
   * this fd 2 (stderr) would be duplicated into fd 1, thus making both
   * stdout and stderr go to the same fd, which was not the intention. */
  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];
    if (use_fd < 0 || use_fd >= fd)
      continue;
    pipes[fd][1] = fcntl(use_fd, F_DUPFD, stdio_count);
    if (pipes[fd][1] == -1)
      uv__write_errno(error_fd);
  }

  for (fd = 0; fd < stdio_count; fd++) {
    close_fd = pipes[fd][0];
    use_fd = pipes[fd][1];

    if (use_fd < 0) {
      if (fd >= 3)
        continue;
      else {
        /* redirect stdin, stdout and stderr to /dev/null even if UV_IGNORE is
         * set
         */
        use_fd = open("/dev/null", fd == 0 ? O_RDONLY : O_RDWR);
        close_fd = use_fd;

        if (use_fd < 0)
          uv__write_errno(error_fd);
      }
    }

    if (fd == use_fd)
      uv__cloexec_fcntl(use_fd, 0);
    else
      fd = dup2(use_fd, fd);

    if (fd == -1)
      uv__write_errno(error_fd);

    if (fd <= 2)
      uv__nonblock_fcntl(fd, 0);

    if (close_fd >= stdio_count)
      uv__close(close_fd);
  }

  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];

    if (use_fd >= stdio_count)
      uv__close(use_fd);
  }

  if (options->cwd != NULL && chdir(options->cwd))
    uv__write_errno(error_fd);

  if (options->flags & (UV_PROCESS_SETUID | UV_PROCESS_SETGID)) {
    /* When dropping privileges from root, the `setgroups` call will
     * remove any extraneous groups. If we don't call this, then
     * even though our uid has dropped, we may still have groups
     * that enable us to do super-user things. This will fail if we
     * aren't root, so don't bother checking the return value, this
     * is just done as an optimistic privilege dropping function.
     */
    SAVE_ERRNO(setgroups(0, NULL));
  }

  if ((options->flags & UV_PROCESS_SETGID) && setgid(options->gid))
    uv__write_errno(error_fd);

  if ((options->flags & UV_PROCESS_SETUID) && setuid(options->uid))
    uv__write_errno(error_fd);

  if (options->env != NULL) {
    environ = options->env;
  }

  /* Reset signal mask just before exec. */
  sigemptyset(&signewset);
  if (sigprocmask(SIG_SETMASK, &signewset, NULL) != 0)
    abort();

#ifdef __MVS__
  execvpe(options->file, options->args, environ);
#else
  execvp(options->file, options->args);
#endif

  uv__write_errno(error_fd);
  abort();
}
#endif


// JAMLEE: 产生一个新的进程。
int uv_spawn(uv_loop_t* loop,
             uv_process_t* process,
             const uv_process_options_t* options) {
// JAMLEE: apple 的 tv 和 watch 这个用不了的
#if defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH)
  /* fork is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED. */
  return UV_ENOSYS;
#else
  sigset_t signewset;
  sigset_t sigoldset;
  int signal_pipe[2] = { -1, -1 }; // JAMLEE: pipe 是一个数组。用于父子进程通信，解决 close-of-fork 问题
  int pipes_storage[8][2]; // JAMLEE: 存储 pipe 的 fd ，所以这里是 int 类型
  int (*pipes)[2]; // JAMLEE: pipes 是一个【数组指针】，指向一个两个int元素的数组。http://c.biancheng.net/view/368.html。pipes_storage 用 pipes 来指，每次加1会移到下一行
  int stdio_count;
  ssize_t r;
  pid_t pid; // JAMLEE: fork 后得返回值
  int err;
  int exec_errorno;
  int i;
  int status;

  assert(options->file != NULL);
  assert(!(options->flags & ~(UV_PROCESS_DETACHED |
                              UV_PROCESS_SETGID |
                              UV_PROCESS_SETUID |
                              UV_PROCESS_WINDOWS_HIDE |
                              UV_PROCESS_WINDOWS_HIDE_CONSOLE |
                              UV_PROCESS_WINDOWS_HIDE_GUI |
                              UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS)));
  // JAMLEE: 初始化 process handle
  uv__handle_init(loop, (uv_handle_t*)process, UV_PROCESS);
  QUEUE_INIT(&process->queue); // JAMLEE: 初始化 process 的queue。队列queue用途呢？queue 的定义为 void* queue[2]。分别只想上一个和下一个节点（这里是process）

  // JAMLEE: 这里 stdio 设置为3是指 stdin, stdout, stderr 吗？是的。stdio 表示3个标准输入输出
  stdio_count = options->stdio_count; // JAMLEE: 默认情况下，stdio_count 是 0
  if (stdio_count < 3)
    stdio_count = 3;

  err = UV_ENOMEM;
  pipes = pipes_storage; // JAMLEE: pipes 的类型和 pipe_storage。
  if (stdio_count > (int) ARRAY_SIZE(pipes_storage))
    pipes = uv__malloc(stdio_count * sizeof(*pipes)); // JAMLEE: sizeof 也可以对一个值取 size

  if (pipes == NULL)
    goto error;
  
  // JAMLEE: pipes 这个最多有 8 行。所以 i < 8 就可以了。把前三行的数值都设置为 -1 来初始化
  for (i = 0; i < stdio_count; i++) {
    pipes[i][0] = -1;
    pipes[i][1] = -1;
  }

  // JAMLEE: 当前的stdio有3个，所以填充起来3个pipe。options->stdio 这个初始化空间时至少要初始化三个这个谁来定义的呢? 原来默认情况下 options->stdio_count 是 0。这里不执行。
  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_init_stdio(options->stdio + i, pipes[i]); // JAMLEE: options->stdio 感觉是一个未定义的具体值的指针呢？
    if (err)
      goto error;
  }

  // JAMLEE: signal_pipe 解决问题 close-on-exec。 https://blog.csdn.net/Leeds1993/article/details/52724428
  /* This pipe is used by the parent to wait until
   * the child has called `execve()`. We need this
   * to avoid the following race condition:
   *
   *    if ((pid = fork()) > 0) {
   *      kill(pid, SIGTERM);
   *    }
   *    else if (pid == 0) {
   *      execve("/bin/cat", argp, envp);
   *    }
   *
   * The parent sends a signal immediately after forking.
   * Since the child may not have called `execve()` yet,
   * there is no telling what process receives the signal,
   * our fork or /bin/cat.
   *
   * To avoid ambiguity, we create a pipe with both ends
   * marked close-on-exec. Then, after the call to `fork()`,
   * the parent polls the read end until it EOFs or errors with EPIPE.
   */
  err = uv__make_pipe(signal_pipe, 0); // JAMLEE: 创建一个 pipe 出来，fd 放置到 signal_pipe 中。
  if (err)
    goto error;

  uv_signal_start(&loop->child_watcher, uv__chld, SIGCHLD);

  // JAMLEE: 获取到 loop->cloexec_lock 这个读写锁, 防止在 worker threads 打开新的fds
  // 获取和释放之间包含着信号设置函数
  /* Acquire write lock to prevent opening new fds in worker threads */
  uv_rwlock_wrlock(&loop->cloexec_lock);

  // JAMLEE: 启动 child 时, 屏蔽很多信号。sig del set: The sigdelset() function deletes the individual signal specified by signo from the signal set pointed to by set.
  // SIG_BLOCK: 结果集是当前集合参数集的并集
  // SIG_UNBLOCK: 结果集是当前集合参数集的差集
  // SIG_SETMASK: 结果集是由参数集指向的集
  /* Start the child with most signals blocked, to avoid any issues before we
   * can reset them, but allow program failures to exit (and not hang). */
  sigfillset(&signewset); // JAMLEE: 这是 signewset
  sigdelset(&signewset, SIGKILL);
  sigdelset(&signewset, SIGSTOP);
  sigdelset(&signewset, SIGTRAP);
  sigdelset(&signewset, SIGSEGV);
  sigdelset(&signewset, SIGBUS);
  sigdelset(&signewset, SIGILL);
  sigdelset(&signewset, SIGSYS);
  sigdelset(&signewset, SIGABRT);
  if (pthread_sigmask(SIG_BLOCK, &signewset, &sigoldset) != 0) // 阻塞信号
    abort();

  // JAMLEE: fork 出子进程, 返回值： 若成功调用一次则返回两个值，子进程返回0，父进程返回子进程ID；否则，出错返回-1
  pid = fork();
  if (pid == -1) // JAMLEE: fork 失败
    err = UV__ERR(errno);

  if (pid == 0) // JAMLEE: 当前是子进程运行, 本函数不会退出
    uv__process_child_init(options, stdio_count, pipes, signal_pipe[1]);

  // JAMLEE: pid > 0 时运行。也就是在父进程中。
  // JAMLEE: 这里子进程不会运行吗？答案是上面这个函数已sigoldset经是不退出的。退出也是直接 abort。下面是父进程恢复信号处理。
  // APUE: https://anmingyu11.gitbooks.io/unix/content/di-shi-er-zhang-xian-cheng-kong-zhi/128-xian-cheng-he-xin-hao.html
  // int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);
  if (pthread_sigmask(SIG_SETMASK, &sigoldset, NULL) != 0) // 还原主线程的屏蔽状态字。屏蔽状态字用户可以读写，未决状态字用户只能读；这是信号设计机制。
    abort();

  // JAMLEE: 释放loop->cloexec_lock锁
  /* Release lock in parent process */
  uv_rwlock_wrunlock(&loop->cloexec_lock);

  // JAMLEE: 当前是在父进程，为什么关闭了一个文件描述符。
  uv__close(signal_pipe[1]);

  // JAMLEE: 返回 -1 意味这进程没有fork成功
  if (pid == -1) {
    uv__close(signal_pipe[0]);
    goto error;
  }

  process->status = 0;
  exec_errorno = 0;
  do
    r = read(signal_pipe[0], &exec_errorno, sizeof(exec_errorno)); // JAMLEE: 读取 exec_errorno 信息
  while (r == -1 && errno == EINTR);

  if (r == 0)
    ; /* okay, EOF */
  else if (r == sizeof(exec_errorno)) {
    do
      err = waitpid(pid, &status, 0); /* okay, read errorno */
    while (err == -1 && errno == EINTR);
    assert(err == pid);
  } else if (r == -1 && errno == EPIPE) {
    do
      err = waitpid(pid, &status, 0); /* okay, got EPIPE */
    while (err == -1 && errno == EINTR);
    assert(err == pid);
  } else
    abort();

  uv__close_nocheckstdio(signal_pipe[0]);

  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_open_stream(options->stdio + i, pipes[i]);
    if (err == 0)
      continue;

    while (i--)
      uv__process_close_stream(options->stdio + i);

    goto error;
  }

  /* Only activate this handle if exec() happened successfully */
  if (exec_errorno == 0) {
    QUEUE_INSERT_TAIL(&loop->process_handles, &process->queue);
    uv__handle_start(process);
  }

  process->pid = pid;
  process->exit_cb = options->exit_cb;

  if (pipes != pipes_storage)
    uv__free(pipes);

  return exec_errorno;

error:
  if (pipes != NULL) {
    for (i = 0; i < stdio_count; i++) {
      if (i < options->stdio_count)
        if (options->stdio[i].flags & (UV_INHERIT_FD | UV_INHERIT_STREAM))
          continue;
      if (pipes[i][0] != -1)
        uv__close_nocheckstdio(pipes[i][0]);
      if (pipes[i][1] != -1)
        uv__close_nocheckstdio(pipes[i][1]);
    }

    if (pipes != pipes_storage)
      uv__free(pipes);
  }

  return err;
#endif
}


// JAMLEE: 给传入的进程发送信号
int uv_process_kill(uv_process_t* process, int signum) {
  return uv_kill(process->pid, signum);
}


// JAMLEE: 调用kill系统调用
int uv_kill(int pid, int signum) {
  if (kill(pid, signum))
    return UV__ERR(errno);
  else
    return 0;
}

// JAMLEE: 关闭进程
void uv__process_close(uv_process_t* handle) {
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
  if (QUEUE_EMPTY(&handle->loop->process_handles))
    uv_signal_stop(&handle->loop->child_watcher);
}
