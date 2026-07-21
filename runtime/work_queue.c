#include "work_queue.h"
#include <stdlib.h>

void fr_run_queue_init(fr_run_queue_t *q) {
    q->lock = fr_mutex_create();
    q->head = q->tail = NULL;
    q->count = 0;
}

void fr_run_queue_destroy(fr_run_queue_t *q) {
    if (!q) return;
    fr_mutex_lock(q->lock);
    fr_run_node_t *n = q->head;
    while (n) {
        fr_run_node_t *next = n->next;
        free(n);
        n = next;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    fr_mutex_unlock(q->lock);
    fr_mutex_destroy(q->lock);
    q->lock = NULL;
}

void fr_run_queue_push(fr_run_queue_t *q, fr_coro_t *coro) {
    fr_run_node_t *node = (fr_run_node_t *)malloc(sizeof(fr_run_node_t));
    if (!node) return;
    node->coro = coro;
    node->next = NULL;
    fr_mutex_lock(q->lock);
    if (q->tail) q->tail->next = node;
    else q->head = node;
    q->tail = node;
    q->count++;
    fr_mutex_unlock(q->lock);
}

static fr_coro_t *pop_locked(fr_run_queue_t *q) {
    if (!q->head) return NULL;
    fr_run_node_t *node = q->head;
    q->head = node->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    fr_coro_t *coro = node->coro;
    free(node);
    return coro;
}

fr_coro_t *fr_run_queue_pop(fr_run_queue_t *q) {
    fr_mutex_lock(q->lock);
    fr_coro_t *coro = pop_locked(q);
    fr_mutex_unlock(q->lock);
    return coro;
}

fr_coro_t *fr_run_queue_steal(fr_run_queue_t *victim, fr_run_queue_t *thief) {
    (void)thief;
    fr_mutex_lock(victim->lock);
    fr_coro_t *coro = NULL;
    if (victim->tail && victim->head != victim->tail) {
        fr_run_node_t *prev = victim->head;
        while (prev->next && prev->next != victim->tail) prev = prev->next;
        if (prev->next == victim->tail) {
            fr_run_node_t *node = victim->tail;
            victim->tail = prev;
            prev->next = NULL;
            victim->count--;
            coro = node->coro;
            free(node);
        }
    }
    if (!coro) coro = pop_locked(victim);
    fr_mutex_unlock(victim->lock);
    return coro;
}

void fr_native_queue_init(fr_native_queue_t *q) {
    q->lock = fr_mutex_create();
    q->head = q->tail = NULL;
    q->count = 0;
}

void fr_native_queue_destroy(fr_native_queue_t *q) {
    if (!q) return;
    fr_mutex_lock(q->lock);
    fr_native_node_t *n = q->head;
    while (n) {
        fr_native_node_t *next = n->next;
        free(n);
        n = next;
    }
    q->head = q->tail = NULL;
    q->count = 0;
    fr_mutex_unlock(q->lock);
    fr_mutex_destroy(q->lock);
    q->lock = NULL;
}

void fr_native_queue_push(fr_native_queue_t *q, fr_native_fn fn, void *arg) {
    fr_native_node_t *node = (fr_native_node_t *)malloc(sizeof(fr_native_node_t));
    if (!node) return;
    node->fn = fn;
    node->arg = arg;
    node->next = NULL;
    fr_mutex_lock(q->lock);
    if (q->tail) q->tail->next = node;
    else q->head = node;
    q->tail = node;
    q->count++;
    fr_mutex_unlock(q->lock);
}

static fr_native_fn pop_native_locked(fr_native_queue_t *q, void **arg_out, int from_tail) {
    if (!q->head) return NULL;
    fr_native_node_t *node;
    if (!from_tail || q->head == q->tail) {
        node = q->head;
        q->head = node->next;
        if (!q->head) q->tail = NULL;
    } else {
        fr_native_node_t *prev = q->head;
        while (prev->next && prev->next != q->tail) prev = prev->next;
        node = q->tail;
        if (prev->next == q->tail) {
            prev->next = NULL;
            q->tail = prev;
        } else {
            node = q->head;
            q->head = node->next;
            if (!q->head) q->tail = NULL;
        }
    }
    q->count--;
    if (arg_out) *arg_out = node->arg;
    fr_native_fn fn = node->fn;
    free(node);
    return fn;
}

fr_native_fn fr_native_queue_pop(fr_native_queue_t *q, void **arg_out) {
    fr_mutex_lock(q->lock);
    fr_native_fn fn = pop_native_locked(q, arg_out, 0);
    fr_mutex_unlock(q->lock);
    return fn;
}

fr_native_fn fr_native_queue_steal(fr_native_queue_t *victim, void **arg_out) {
    fr_mutex_lock(victim->lock);
    fr_native_fn fn = pop_native_locked(victim, arg_out, 1);
    if (!fn) fn = pop_native_locked(victim, arg_out, 0);
    fr_mutex_unlock(victim->lock);
    return fn;
}
