#ifndef FORGE_WORK_QUEUE_H
#define FORGE_WORK_QUEUE_H

#include "forge/thread.h"
#include <stdatomic.h>
#include <stddef.h>

typedef struct fr_coro fr_coro_t;
typedef void (*fr_native_fn)(void *arg);

/*
 * One mutex-guarded doubly-linked deque backs both work queues in the
 * scheduler: the coroutine run queue and the native-task queue. They differ
 * only in what a node carries, so the list mechanics live in a single
 * implementation and the two public APIs below are thin typed wrappers.
 *
 * The owner pushes at the tail and pops at the head (FIFO); a thief takes
 * from the tail, so stealing touches the opposite end from the owner's
 * dequeue. The list is doubly linked purely to make that tail removal O(1)
 * -- the previous singly-linked version walked the whole queue to find the
 * tail's predecessor while holding the victim's lock, so a busy victim paid
 * O(n) per steal attempt.
 *
 * Popped nodes go onto a small per-queue free list instead of back to
 * malloc, which keeps the steady-state push/pop path allocation-free (the
 * io_uring HTTP dispatch path pushes one native task per request).
 */

typedef struct {
    fr_coro_t *coro;   /* run queue: the coroutine to resume */
    fr_native_fn fn;   /* native queue: the task entry point */
    void *arg;         /* native queue: the task argument */
} fr_wq_item_t;

typedef struct fr_wq_node {
    fr_wq_item_t item;
    struct fr_wq_node *prev;
    struct fr_wq_node *next;
} fr_wq_node_t;

typedef struct {
    fr_mutex_t *lock;
    fr_wq_node_t *head;
    fr_wq_node_t *tail;
    fr_wq_node_t *spare;
    size_t spare_count;
    /* Mutated only under `lock`, but read unlocked by the scheduler's "is
     * there anything to do?" probe -- atomic so that read is defined rather
     * than a data race. A stale value there is harmless: it only costs the
     * worker one extra loop iteration before it parks. */
    _Atomic size_t count;
} fr_wq_t;

typedef fr_wq_t fr_run_queue_t;
typedef fr_wq_t fr_native_queue_t;

void fr_run_queue_init(fr_run_queue_t *q);
void fr_run_queue_destroy(fr_run_queue_t *q);
void fr_run_queue_push(fr_run_queue_t *q, fr_coro_t *coro);
fr_coro_t *fr_run_queue_pop(fr_run_queue_t *q);
fr_coro_t *fr_run_queue_steal(fr_run_queue_t *victim);

void fr_native_queue_init(fr_native_queue_t *q);
void fr_native_queue_destroy(fr_native_queue_t *q);
int fr_native_queue_push(fr_native_queue_t *q, fr_native_fn fn, void *arg);
size_t fr_native_queue_count(fr_native_queue_t *q);
fr_native_fn fr_native_queue_pop(fr_native_queue_t *q, void **arg_out);
fr_native_fn fr_native_queue_steal(fr_native_queue_t *victim, void **arg_out);

#endif
