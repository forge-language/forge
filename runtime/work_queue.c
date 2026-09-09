#include "work_queue.h"
#include <stdlib.h>

/* Cap on recycled nodes kept per queue. Large enough that a steady-state
 * producer/consumer pair never calls malloc, small enough that a burst
 * doesn't pin much memory once the queue drains. */
#define FR_WQ_SPARE_MAX ((size_t)64)

/* ---------------------------------------------------------------------- *
 * Generic deque -- every helper below must be called with q->lock held.
 * ---------------------------------------------------------------------- */

/* count is atomic only so the scheduler can probe it without the lock; every
 * access in here already holds the lock, so relaxed ordering is enough and
 * keeps these as plain loads/stores. */
static size_t wq_count(fr_wq_t *q) {
    return atomic_load_explicit(&q->count, memory_order_relaxed);
}

static void wq_count_set(fr_wq_t *q, size_t v) {
    atomic_store_explicit(&q->count, v, memory_order_relaxed);
}

static fr_wq_node_t *wq_node_take(fr_wq_t *q) {
    fr_wq_node_t *n = q->spare;
    if (!n) return (fr_wq_node_t *)malloc(sizeof(fr_wq_node_t));
    q->spare = n->next;
    q->spare_count--;
    return n;
}

static void wq_node_recycle(fr_wq_t *q, fr_wq_node_t *n) {
    if (q->spare_count >= FR_WQ_SPARE_MAX) {
        free(n);
        return;
    }
    n->next = q->spare;
    q->spare = n;
    q->spare_count++;
}

static int wq_push_tail(fr_wq_t *q, fr_wq_item_t item) {
    fr_wq_node_t *node = wq_node_take(q);
    if (!node) return -1;
    node->item = item;
    node->next = NULL;
    node->prev = q->tail;
    if (q->tail) q->tail->next = node;
    else q->head = node;
    q->tail = node;
    wq_count_set(q, wq_count(q) + 1);
    return 0;
}

/* Removes one node from the requested end and reports its payload. Returns 0
 * when the queue is empty, leaving *out untouched. */
static int wq_pop(fr_wq_t *q, int from_tail, fr_wq_item_t *out) {
    fr_wq_node_t *node = from_tail ? q->tail : q->head;
    if (!node) return 0;
    if (node->prev) node->prev->next = node->next;
    else q->head = node->next;
    if (node->next) node->next->prev = node->prev;
    else q->tail = node->prev;
    wq_count_set(q, wq_count(q) - 1);
    *out = node->item;
    wq_node_recycle(q, node);
    return 1;
}

static void wq_init(fr_wq_t *q) {
    q->lock = fr_mutex_create();
    q->head = q->tail = NULL;
    q->spare = NULL;
    q->spare_count = 0;
    wq_count_set(q, 0);
}

static void wq_free_chain(fr_wq_node_t *n) {
    while (n) {
        fr_wq_node_t *next = n->next;
        free(n);
        n = next;
    }
}

static void wq_destroy(fr_wq_t *q) {
    if (!q || !q->lock) return;
    fr_mutex_lock(q->lock);
    wq_free_chain(q->head);
    wq_free_chain(q->spare);
    q->head = q->tail = NULL;
    q->spare = NULL;
    q->spare_count = 0;
    wq_count_set(q, 0);
    fr_mutex_unlock(q->lock);
    fr_mutex_destroy(q->lock);
    q->lock = NULL;
}

/* ---------------------------------------------------------------------- *
 * Coroutine run queue
 * ---------------------------------------------------------------------- */

void fr_run_queue_init(fr_run_queue_t *q) { wq_init(q); }

void fr_run_queue_destroy(fr_run_queue_t *q) { wq_destroy(q); }

void fr_run_queue_push(fr_run_queue_t *q, fr_coro_t *coro) {
    fr_wq_item_t item = { coro, NULL, NULL };
    fr_mutex_lock(q->lock);
    wq_push_tail(q, item);
    fr_mutex_unlock(q->lock);
}

static fr_coro_t *run_queue_take(fr_run_queue_t *q, int from_tail) {
    fr_wq_item_t item;
    fr_mutex_lock(q->lock);
    int got = wq_pop(q, from_tail, &item);
    fr_mutex_unlock(q->lock);
    return got ? item.coro : NULL;
}

fr_coro_t *fr_run_queue_pop(fr_run_queue_t *q) { return run_queue_take(q, 0); }

fr_coro_t *fr_run_queue_steal(fr_run_queue_t *victim) { return run_queue_take(victim, 1); }

/* ---------------------------------------------------------------------- *
 * Native-task queue
 * ---------------------------------------------------------------------- */

void fr_native_queue_init(fr_native_queue_t *q) { wq_init(q); }

void fr_native_queue_destroy(fr_native_queue_t *q) { wq_destroy(q); }

int fr_native_queue_push(fr_native_queue_t *q, fr_native_fn fn, void *arg) {
    fr_wq_item_t item = { NULL, fn, arg };
    fr_mutex_lock(q->lock);
    int rc = wq_push_tail(q, item);
    fr_mutex_unlock(q->lock);
    return rc;
}

size_t fr_native_queue_count(fr_native_queue_t *q) {
    if (!q || !q->lock) return 0;
    fr_mutex_lock(q->lock);
    size_t n = wq_count(q);
    fr_mutex_unlock(q->lock);
    return n;
}

static fr_native_fn native_queue_take(fr_native_queue_t *q, int from_tail, void **arg_out) {
    fr_wq_item_t item;
    fr_mutex_lock(q->lock);
    int got = wq_pop(q, from_tail, &item);
    fr_mutex_unlock(q->lock);
    if (!got) return NULL;
    if (arg_out) *arg_out = item.arg;
    return item.fn;
}

fr_native_fn fr_native_queue_pop(fr_native_queue_t *q, void **arg_out) {
    return native_queue_take(q, 0, arg_out);
}

fr_native_fn fr_native_queue_steal(fr_native_queue_t *victim, void **arg_out) {
    return native_queue_take(victim, 1, arg_out);
}
