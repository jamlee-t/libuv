/* Copyright (c) 2013, Ben Noordhuis <info@bnoordhuis.nl>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef QUEUE_H_
#define QUEUE_H_

#include <stddef.h>

typedef void *QUEUE[2]; // JAMLEE: QUEUE 类型定义。为2个元素的数组。它是一个具有2个元素的数组，第一个元素指向下一个队列元素，第二个元素指向上一个队列元素，其实它就是Libuv中的队列核心。

// JAMLEE: 仅在库内部使用的宏
// 取地址 &((*(q))[0])), 这里等于是数据中元素的地址, 类似于 &a[0]
/* Private macros. */
#define QUEUE_NEXT(q)       (*(QUEUE **) &((*(q))[0]))
#define QUEUE_PREV(q)       (*(QUEUE **) &((*(q))[1]))

// JAMLEE: QUEUE_PREV_NEXT 前一节点的下一节点，QUEUE_NEXT_PREV 下一节点的前一节点
#define QUEUE_PREV_NEXT(q)  (QUEUE_NEXT(QUEUE_PREV(q)))
#define QUEUE_NEXT_PREV(q)  (QUEUE_PREV(QUEUE_NEXT(q)))

/* Public macros. */
#define QUEUE_DATA(ptr, type, field)                                          \
  ((type *) ((char *) (ptr) - offsetof(type, field)))

/* Important note: mutating the list while QUEUE_FOREACH is
 * iterating over its elements results in undefined behavior.
 */
#define QUEUE_FOREACH(q, h)                                                   \
  for ((q) = QUEUE_NEXT(h); (q) != (h); (q) = QUEUE_NEXT(q))

// JAMLEE: 是否是空队列
#define QUEUE_EMPTY(q)                                                        \
  ((const QUEUE *) (q) == (const QUEUE *) QUEUE_NEXT(q))

// JAMLEE: 当前队列。也就是下一个元素是头。
#define QUEUE_HEAD(q)                                                         \
  (QUEUE_NEXT(q))

// JAMLEE: 调用 QUEUE_NEXT 和 QUEUE_PREV。用 do while 将其包装为 1 个语句。
// 表示将[0]、[1]上的元素都赋于自身的地址。传入的q 应该是某个结构体上的 queue 字段。
// 比如：UV_PROCESS_PRIVATE_FIELDS 就含有 queue, 所有多个 process 可以组织为 1 个队列（循环链表形式）
// QUEUE_NEXT(q) = (q); 变为 (*(QUEUE **) &((*(q))[0])) = q
#define QUEUE_INIT(q)                                                         \
  do {                                                                        \
    QUEUE_NEXT(q) = (q);                                                      \
    QUEUE_PREV(q) = (q);                                                      \
  }                                                                           \
  while (0)

// JAMLEE: QUEUE_ADD 将元素 n 加到 h 的前面。所以是入队。假设有且仅有两个节点
// 1. h 的前一个节点的下一节点为 n，也即是 h 的下一节点为n。h 的前一点等于自身。
// 2. n QUEUE_NEXT_PREV 下一节点的前一节点为 h。也就是 n 的前一节点为 h。n的下一节点为自身。
// 3. h 的前一节也是h自身，赋值为 n
// 4. h 的前一节点的下一节点也就是n的下一节点赋值为 h.
// 链表首尾相连。插入链表，以插入元素为核心分为前后、后前、前、后四步。把键设置正确。
#define QUEUE_ADD(h, n)                                                       \
  do {                                                                        \
    QUEUE_PREV_NEXT(h) = QUEUE_NEXT(n);                                       \
    QUEUE_NEXT_PREV(n) = QUEUE_PREV(h);                                       \
    QUEUE_PREV(h) = QUEUE_PREV(n);                                            \
    QUEUE_PREV_NEXT(h) = (h);                                                 \
  }                                                                           \
  while (0)

// JAMLEE：QUEUE_SPLIT 。这个宏定义函数就是为了把h删了，把n加到原来到h的位置，其中q是属于h之前的队列q是head。
// head 一般是指 h 的下一个节点。
#define QUEUE_SPLIT(h, q, n)                                                  \
  do {                                                                        \
    QUEUE_PREV(n) = QUEUE_PREV(h);                                            \
    QUEUE_PREV_NEXT(n) = (n);                                                 \
    QUEUE_NEXT(n) = (q);                                                      \
    QUEUE_PREV(h) = QUEUE_PREV(q);                                            \
    QUEUE_PREV_NEXT(h) = (h);                                                 \
    QUEUE_PREV(q) = (n);                                                      \
  }                                                                           \
  while (0)

// JAMLEE：QUEUE_MOVE。h 是空队列（只有 h 本身一个元素） 就初始化 n 为空。把 h 替换为 n。
// 把 h 删掉。用 n 来替换。
#define QUEUE_MOVE(h, n)                                                      \
  do {                                                                        \
    if (QUEUE_EMPTY(h))                                                       \
      QUEUE_INIT(n);                                                          \
    else {                                                                    \
      QUEUE* q = QUEUE_HEAD(h);                                               \
      QUEUE_SPLIT(h, q, n);                                                   \
    }                                                                         \
  }                                                                           \
  while (0)

#define QUEUE_INSERT_HEAD(h, q)                                               \
  do {                                                                        \
    QUEUE_NEXT(q) = QUEUE_NEXT(h);                                            \
    QUEUE_PREV(q) = (h);                                                      \
    QUEUE_NEXT_PREV(q) = (q);                                                 \
    QUEUE_NEXT(h) = (q);                                                      \
  }                                                                           \
  while (0)

// JAMLEE: 在队列尾部插入
#define QUEUE_INSERT_TAIL(h, q)                                               \
  do {                                                                        \
    QUEUE_NEXT(q) = (h);                                                      \
    QUEUE_PREV(q) = QUEUE_PREV(h);                                            \
    QUEUE_PREV_NEXT(q) = (q);                                                 \
    QUEUE_PREV(h) = (q);                                                      \
  }                                                                           \
  while (0)

// JAMLEE: 从队列中移除 q 元素
#define QUEUE_REMOVE(q)                                                       \
  do {                                                                        \
    QUEUE_PREV_NEXT(q) = QUEUE_NEXT(q);                                       \
    QUEUE_NEXT_PREV(q) = QUEUE_PREV(q);                                       \
  }                                                                           \
  while (0)

#endif /* QUEUE_H_ */
