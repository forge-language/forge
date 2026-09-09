#include "forge_runtime.h"
#include "work_queue.h"
#include "forge/arena.h"
#include "forge/event.h"
#include "forge/platform.h"
#include "forge/thread.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static char *fr_strdup(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

#define MAILBOX_CAP 256

typedef struct fr_mailbox {
    fr_msg_t msgs[MAILBOX_CAP];
    size_t head;
    size_t tail;
    size_t count;
} fr_mailbox_t;

struct fr_coro {
    int id;
    fr_coro_fn fn;
    void *state;
    size_t state_size;
    /* status and await_ready are written by whichever thread currently
     * "owns" this coroutine (see on_queue below), but read from other
     * threads too (e.g. process_has_active() from the poller thread, or
     * event_resume_cb() checking in on a wakeup) -- _Atomic makes those
     * cross-thread accesses well-defined instead of a plain data race. */
    _Atomic fr_coro_status_t status;
    int step;
    /* Tracks whether this coroutine is currently "claimed" by a worker
     * (either sitting in a run queue, or actively being executed by
     * worker_main). It must stay non-zero for the coroutine's *entire*
     * execution window -- including any fr_await_fd registration it makes
     * -- so that a concurrent event_resume_cb() cannot re-enqueue (and
     * thus double-execute) a coroutine that is still running. See
     * worker_main() and event_resume_cb() for the full protocol. */
    atomic_int on_queue;
    fr_process_t *proc;
    int await_fd;
    uint32_t await_events;
    _Atomic int await_ready;
    struct fr_coro *next;
    /* Per-coroutine scratch arena (see fr_arena_tls()). Bound to the
     * coroutine rather than the OS thread it happens to run on, since
     * work-stealing can move a coroutine between worker threads between
     * steps. Lazily created on first use; freed in fr_process_destroy. */
    fr_arena_t *arena;
};

struct fr_process {
    char *name;
    fr_scheduler_t *sched;
    fr_mailbox_t mailbox;
    fr_coro_t *coros;
    fr_coro_t *coro_tail;
    int next_coro_id;
    fr_coro_fn receive_handler;
    size_t receive_state_size;
    int is_supervisor;
    fr_restart_policy_t restart_policy;
    fr_process_t **children;
    size_t child_count;
    int worker_id;
    fr_mutex_t *lock;
    fr_cond_t *msg_cond;
    struct fr_process *next;
};

struct fr_scheduler {
    fr_process_t *processes;
    int worker_count;
    /* running/workers_started/native_pending are read by workers without
     * sched->lock (the pre-park "is there anything to do?" probe), so they
     * are atomic to make those reads defined rather than a data race that
     * the compiler is free to hoist out of the worker loop. native_pending
     * is still mutated only under sched->lock -- see fr_sched_pool_submit
     * for why that lock, not the atomicity, is what prevents lost wakeups.
     * done is touched only by the fr_scheduler_run thread. */
    atomic_int running;
    int done;
    atomic_int workers_started;
    atomic_int native_pending;
    fr_thread_t **workers;
    fr_run_queue_t *worker_queues;
    fr_native_queue_t *native_queues;
    fr_event_loop_t *event_loop;
    fr_mutex_t *lock;
    fr_cond_t *idle_cond;
};

static _Thread_local fr_coro_t *tls_current_coro = NULL;
static _Thread_local int tls_worker_id = -1;
static fr_scheduler_t *g_global_sched = NULL;

fr_coro_t *fr_coro_current(void) {
    return tls_current_coro;
}

fr_arena_t *fr_coro_get_arena(fr_coro_t *coro) {
    if (!coro) return NULL;
    if (!coro->arena) coro->arena = fr_arena_create(0);
    return coro->arena;
}

/* Bridges fr_arena_tls()/fr_arena_tls_reset() (called from stdlib code that
 * has no scheduler context of its own) to the current coroutine's own
 * arena, so a pointer allocated before a yield stays valid -- and isn't
 * silently aliased by an unrelated coroutine's str_reset_arena() -- after
 * work-stealing resumes this coroutine on a different worker thread. */
static fr_arena_t *scheduler_coro_arena_provider(void) {
    return fr_coro_get_arena(tls_current_coro);
}

static void scheduler_register_arena_provider(void) {
    static atomic_int registered;
    int expected = 0;
    if (atomic_compare_exchange_strong(&registered, &expected, 1)) {
        fr_arena_set_coro_provider(scheduler_coro_arena_provider);
    }
}

fr_scheduler_t *fr_scheduler_global(void) {
    return g_global_sched;
}

void fr_scheduler_set_global(fr_scheduler_t *sched) {
    g_global_sched = sched;
}

int fr_sched_pool_available(void) {
    return g_global_sched && g_global_sched->running && g_global_sched->workers_started;
}

static int default_worker_count(int n) {
    /* fr_platform_cpu_count() already substitutes a sane default when the OS
     * won't report a count. */
    return n > 0 ? n : fr_platform_cpu_count();
}

/* ---------------------------------------------------------------------- *
 * Process mailbox -- ring buffer, always accessed under proc->lock.
 * ---------------------------------------------------------------------- */

static int mailbox_push(fr_mailbox_t *mb, fr_msg_t msg) {
    if (mb->count >= MAILBOX_CAP) return 0;
    mb->msgs[mb->tail] = msg;
    mb->tail = (mb->tail + 1) % MAILBOX_CAP;
    mb->count++;
    return 1;
}

static int mailbox_pop(fr_mailbox_t *mb, fr_msg_t *out) {
    if (mb->count == 0) return 0;
    *out = mb->msgs[mb->head];
    mb->head = (mb->head + 1) % MAILBOX_CAP;
    mb->count--;
    return 1;
}

/* ---------------------------------------------------------------------- *
 * Run-queue handoff. The on_queue CAS is the single gate through which a
 * coroutine passes from "nobody owns it" to "exactly one worker owns it";
 * every path that wants to schedule a coroutine goes through enqueue_coro.
 * ---------------------------------------------------------------------- */

static void enqueue_coro(fr_scheduler_t *sched, fr_coro_t *coro) {
    if (!coro) return;
    if (coro->status == FR_CORO_DONE || coro->status == FR_CORO_ERROR) return;
    /* Atomic check-and-set: only the thread that wins the 0->1 transition
     * may push this coroutine onto a run queue. This prevents two racing
     * callers (e.g. event_resume_cb on the event-loop thread and a worker
     * re-enqueuing after a run step) from both observing "not on queue"
     * and double-scheduling the same coroutine. */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&coro->on_queue, &expected, 1)) return;
    int wid = coro->proc->worker_id % sched->worker_count;
    fr_run_queue_push(&sched->worker_queues[wid], coro);
    fr_cond_broadcast(sched->idle_cond);
}

static void event_resume_cb(fr_event_loop_t *loop, int fd, uint32_t events, void *userdata) {
    (void)loop;
    (void)fd;
    (void)events;
    fr_coro_t *coro = (fr_coro_t *)userdata;
    if (!coro) return;
    /* Deliberately do NOT touch coro->status here. This callback can run
     * concurrently (on the poller thread) with the coroutine still being
     * actively executed by a worker thread -- writing status directly
     * from here raced with run_coro_step()'s own status write and could
     * hand the coroutine to a second worker while the first was still
     * mid-execution (double-execution / state corruption). Only the
     * worker that currently owns the coroutine (tracked via on_queue) is
     * allowed to transition status; this callback just records that the
     * wakeup happened and attempts the handoff via the same CAS gate
     * everyone else uses. If the owning worker hasn't released on_queue
     * yet, this enqueue_coro() call is a harmless no-op -- the owner
     * rechecks await_ready itself right before releasing ownership, so
     * the wakeup is never lost (see worker_main). */
    coro->await_ready = 1;
    if (coro->proc && coro->proc->sched) {
        enqueue_coro(coro->proc->sched, coro);
    }
}

/* ---------------------------------------------------------------------- *
 * Coroutine execution
 * ---------------------------------------------------------------------- */

/* Steps a coroutine that this thread owns, once, and publishes the status it
 * settled on. Only the owning worker may call this. */
static void run_coro_step(fr_coro_t *c) {
    tls_current_coro = c;
    fr_coro_status_t st = c->fn(c, c->state);
    tls_current_coro = NULL;
    /* A yield is a voluntary reschedule point, not a suspension -- the
     * coroutine is still runnable, so publish RUNNING directly rather than
     * letting observers see a transient YIELDED. */
    c->status = (st == FR_CORO_YIELDED) ? FR_CORO_RUNNING : st;
}

static int process_has_active(fr_process_t *p) {
    /* proc->lock guards the coros linked-list shape itself (fr_coro_spawn
     * can append to it concurrently from a coroutine running on another
     * worker); the individual c->status reads are already race-free since
     * status is _Atomic. */
    fr_mutex_lock(p->lock);
    int active = 0;
    for (fr_coro_t *c = p->coros; c; c = c->next) {
        if (c->status != FR_CORO_DONE && c->status != FR_CORO_ERROR) { active = 1; break; }
    }
    fr_mutex_unlock(p->lock);
    return active;
}

static int sched_any_active(fr_scheduler_t *sched) {
    for (fr_process_t *p = sched->processes; p; p = p->next) {
        if (process_has_active(p)) return 1;
    }
    return 0;
}

static void scan_enqueue_runnable(fr_scheduler_t *sched) {
    for (fr_process_t *p = sched->processes; p; p = p->next) {
        fr_mutex_lock(p->lock);
        for (fr_coro_t *c = p->coros; c; c = c->next) {
            if (c->status == FR_CORO_RUNNING) {
                enqueue_coro(sched, c);
            }
        }
        fr_mutex_unlock(p->lock);
    }
}

/* ---------------------------------------------------------------------- *
 * Worker threads
 * ---------------------------------------------------------------------- */

/* Steps one coroutine takes before a worker puts it back on the queue, so a
 * tight non-yielding loop can't monopolise its worker. */
#define FR_REDUCTION_BUDGET 2000

typedef struct {
    fr_scheduler_t *sched;
    int wid;
} fr_worker_arg_t;

/* True if there is a runnable coroutine or native task sitting in a queue
 * right now -- i.e. something a worker could immediately dequeue and run. */
static int sched_has_queued_work(fr_scheduler_t *sched) {
    if (sched->native_pending > 0) return 1;
    for (int i = 0; i < sched->worker_count; i++) {
        if (sched->worker_queues[i].count > 0) return 1;
        if (sched->native_queues[i].count > 0) return 1;
    }
    return 0;
}

/* True if there is queued work OR any coroutine that is merely alive
 * (including ones parked on WAITING_IO/WAITING_RECV). Used to decide
 * whether the scheduler as a whole is done, not whether a worker has
 * something to do right now -- a coroutine waiting on IO doesn't become
 * queued work until its fd fires or a message arrives. */
static int sched_has_work(fr_scheduler_t *sched) {
    if (sched_has_queued_work(sched)) return 1;
    return sched_any_active(sched);
}

static void *worker_main(void *arg) {
    fr_worker_arg_t *wa = (fr_worker_arg_t *)arg;
    fr_scheduler_t *sched = wa->sched;
    int wid = wa->wid;
    free(wa);

    int cpus = fr_platform_cpu_count();
    if (cpus > 0) fr_thread_pin_cpu(wid % cpus);
    tls_worker_id = wid;

    while (sched->running) {
        fr_coro_t *coro = fr_run_queue_pop(&sched->worker_queues[wid]);
        void *narg = NULL;
        fr_native_fn nfn = NULL;
        if (!coro) {
            /* Take our own native task before paying for a cross-worker
             * coroutine steal scan. That scan acquires one victim lock per
             * peer worker, and for a pool that only ever runs native tasks
             * -- the io_uring HTTP dispatch pool (fr_http_serve_uring) is
             * exactly that -- the coroutine run-queues are permanently
             * empty, so it found nothing on every single dequeue while
             * costing worker_count-1 lock round-trips. fr_sched_pool_submit
             * round-robins into the per-worker native queues, so the
             * common case is a hit right here with no locks at all beyond
             * this queue's own.
             *
             * This does reorder priority: a worker holding a local native
             * task now runs it instead of stealing a peer's coroutine.
             * Both are work-conserving; preferring local work is the
             * standard choice and avoids the pathological case above. */
            nfn = fr_native_queue_pop(&sched->native_queues[wid], &narg);
            if (!nfn) {
                for (int i = 0; i < sched->worker_count; i++) {
                    if (i == wid) continue;
                    coro = fr_run_queue_steal(&sched->worker_queues[i]);
                    if (coro) break;
                }
            }
        }

        if (coro) {
            /* This worker now exclusively owns the coroutine. Unlike
             * before, on_queue is *not* released yet -- it stays non-zero
             * for this entire block, including any fr_await_fd()
             * registration run_coro_step() may perform. That is what
             * prevents event_resume_cb() (running concurrently on the
             * poller thread) from re-enqueuing this same coroutine onto
             * another worker's queue while it is still being executed
             * here, which used to cause double-execution / state
             * corruption. on_queue is only released just below, after
             * this worker is done with it for now. */
            if ((coro->status == FR_CORO_WAITING_IO || coro->status == FR_CORO_WAITING_RECV) &&
                coro->await_ready) {
                /* Its wakeup condition was already satisfied by the time
                 * we got to it (e.g. the fd was immediately readable) --
                 * resume it. Only this owning worker ever makes this
                 * transition. */
                coro->status = FR_CORO_RUNNING;
            }
            int budget = FR_REDUCTION_BUDGET;
            while (budget-- > 0) {
                if (coro->status != FR_CORO_RUNNING) break;
                run_coro_step(coro);
                if (coro->status == FR_CORO_WAITING_IO || coro->status == FR_CORO_WAITING_RECV) break;
                if (coro->status == FR_CORO_DONE || coro->status == FR_CORO_ERROR) break;
            }
            /* Release ownership. Only from this point can a concurrent
             * event_resume_cb() win the on_queue CAS and re-enqueue this
             * coroutine -- guaranteeing any fr_await_fd() registration
             * performed above has already fully completed. */
            atomic_store(&coro->on_queue, 0);
            if (coro->status == FR_CORO_RUNNING) {
                enqueue_coro(sched, coro);
            } else if ((coro->status == FR_CORO_WAITING_IO || coro->status == FR_CORO_WAITING_RECV) &&
                       coro->await_ready) {
                /* Lost-wakeup guard: the event fired while we still held
                 * on_queue (i.e. event_resume_cb's own enqueue_coro()
                 * call above was a guaranteed-fail CAS against our still-
                 * held ownership). Finish the handoff ourselves now that
                 * ownership is released, so the wakeup is never dropped. */
                coro->status = FR_CORO_RUNNING;
                enqueue_coro(sched, coro);
            }
            continue;
        }

        if (!nfn) {
            for (int i = 0; i < sched->worker_count; i++) {
                if (i == wid) continue;
                nfn = fr_native_queue_steal(&sched->native_queues[i], &narg);
                if (nfn) break;
            }
        }
        if (nfn) {
            fr_mutex_lock(sched->lock);
            if (sched->native_pending > 0) sched->native_pending--;
            fr_mutex_unlock(sched->lock);
            nfn(narg);
            continue;
        }

        if (!sched_has_queued_work(sched)) {
            /* Nothing immediately runnable. Coroutines that are merely
             * WAITING_IO/WAITING_RECV don't count as queued work -- they
             * become queued (via enqueue_coro's fr_cond_broadcast) only
             * when their fd fires or a message arrives, so it's safe to
             * park here instead of busy-spinning on fr_thread_yield(). */
            if (!sched->running) break;
            fr_mutex_lock(sched->lock);
            if (!sched_has_queued_work(sched)) {
                if (!sched->running) {
                    fr_mutex_unlock(sched->lock);
                    break;
                }
                fr_cond_wait(sched->idle_cond, sched->lock);
            }
            fr_mutex_unlock(sched->lock);
            continue;
        }
        /* Queued work exists somewhere but we raced with another worker
         * (stealing) and came up empty this iteration -- brief yield and
         * retry rather than a hard spin. */
        fr_thread_yield();
    }
    tls_worker_id = -1;
    return NULL;
}

/* ---------------------------------------------------------------------- *
 * Scheduler lifecycle
 * ---------------------------------------------------------------------- */

fr_scheduler_t *fr_scheduler_create(int worker_count) {
    scheduler_register_arena_provider();
    fr_scheduler_t *s = (fr_scheduler_t *)calloc(1, sizeof(fr_scheduler_t));
    if (!s) return NULL;
    s->worker_count = default_worker_count(worker_count);
    s->event_loop = fr_event_loop_create();
    s->lock = fr_mutex_create();
    s->idle_cond = fr_cond_create();
    s->worker_queues = (fr_run_queue_t *)calloc((size_t)s->worker_count, sizeof(fr_run_queue_t));
    s->native_queues = (fr_native_queue_t *)calloc((size_t)s->worker_count, sizeof(fr_native_queue_t));
    s->workers = (fr_thread_t **)calloc((size_t)s->worker_count, sizeof(fr_thread_t *));
    /* Every one of these is load-bearing: without lock and idle_cond the
     * mutex and condvar calls degrade to silent no-ops and the workers run
     * unsynchronized, and without an event loop no coroutine can ever be
     * woken from fr_await_fd. Fail the whole construction instead. */
    if (!s->event_loop || !s->lock || !s->idle_cond ||
        !s->worker_queues || !s->native_queues || !s->workers) {
        fr_scheduler_destroy(s);
        return NULL;
    }
    fr_event_loop_set_cb(s->event_loop, event_resume_cb);
    for (int i = 0; i < s->worker_count; i++) {
        fr_run_queue_init(&s->worker_queues[i]);
        fr_native_queue_init(&s->native_queues[i]);
    }
    return s;
}

/* Clears `running` and wakes every parked worker.
 *
 * The store must happen under sched->lock: a worker parks by re-checking
 * `running` and the queues while holding that lock and then calling
 * fr_cond_wait, which only releases the lock atomically as it goes to sleep.
 * Clearing the flag outside the lock let both the store and the broadcast
 * land in the window between that re-check and the worker actually being
 * asleep -- the broadcast then reached no waiter, the worker slept on a flag
 * that was already false, and the subsequent join hung forever. */
static void scheduler_signal_stop(fr_scheduler_t *sched) {
    fr_mutex_lock(sched->lock);
    atomic_store(&sched->running, 0);
    fr_mutex_unlock(sched->lock);
    fr_cond_broadcast(sched->idle_cond);
}

void fr_scheduler_destroy(fr_scheduler_t *sched) {
    if (!sched) return;
    if (sched->lock) scheduler_signal_stop(sched);
    if (sched->workers) {
        for (int i = 0; i < sched->worker_count; i++) {
            if (sched->workers[i]) fr_thread_join(sched->workers[i]);
        }
    }
    if (sched->worker_queues) {
        for (int i = 0; i < sched->worker_count; i++) {
            fr_run_queue_destroy(&sched->worker_queues[i]);
        }
    }
    if (sched->native_queues) {
        for (int i = 0; i < sched->worker_count; i++) {
            fr_native_queue_destroy(&sched->native_queues[i]);
        }
    }
    fr_process_t *p = sched->processes;
    while (p) {
        fr_process_t *next = p->next;
        fr_process_destroy(p);
        p = next;
    }
    fr_event_loop_destroy(sched->event_loop);
    fr_mutex_destroy(sched->lock);
    fr_cond_destroy(sched->idle_cond);
    free(sched->worker_queues);
    free(sched->native_queues);
    free(sched->workers);
    free(sched);
}

static void scheduler_start_workers(fr_scheduler_t *sched) {
    if (!sched || sched->workers_started) return;
    sched->workers_started = 1;
    for (int i = 0; i < sched->worker_count; i++) {
        fr_worker_arg_t *wa = (fr_worker_arg_t *)malloc(sizeof(fr_worker_arg_t));
        if (!wa) continue;
        wa->sched = sched;
        wa->wid = i;
        fr_thread_start(&sched->workers[i], worker_main, wa);
    }
}

void fr_scheduler_start(fr_scheduler_t *sched) {
    if (!sched) return;
    sched->running = 1;
    sched->done = 0;
    fr_scheduler_set_global(sched);
    scan_enqueue_runnable(sched);
    scheduler_start_workers(sched);
}

void fr_scheduler_stop(fr_scheduler_t *sched) {
    if (!sched) return;
    scheduler_signal_stop(sched);
    for (int i = 0; i < sched->worker_count; i++) {
        if (sched->workers[i]) fr_thread_join(sched->workers[i]);
        sched->workers[i] = NULL;
    }
    atomic_store(&sched->workers_started, 0);
    if (g_global_sched == sched) g_global_sched = NULL;
}

/* ---------------------------------------------------------------------- *
 * Native task pool -- runs plain C callbacks on the same worker threads as
 * coroutines, for blocking work that has no coroutine representation
 * (thread.spawn, the io_uring HTTP dispatch path).
 * ---------------------------------------------------------------------- */

int fr_sched_pool_submit(fr_scheduler_t *sched, void (*fn)(void *), void *arg) {
    if (!sched || !fn) return -1;
    static atomic_int next_q;
    int q = atomic_fetch_add_explicit(&next_q, 1, memory_order_relaxed) % sched->worker_count;
    if (fr_native_queue_push(&sched->native_queues[q], fn, arg) != 0) return -1;
    fr_mutex_lock(sched->lock);
    sched->native_pending++;
    fr_mutex_unlock(sched->lock);
    /* One task needs one worker: signal, don't broadcast. A broadcast here
     * woke every parked worker for each submitted task -- a thundering herd
     * where all but one immediately failed every queue scan and parked
     * again, and the HTTP io_uring path submits once per request.
     *
     * Signalling cannot lose a wakeup: native_pending is incremented above
     * under sched->lock, and worker_main only parks after re-checking
     * sched_has_queued_work() (which reports native_pending > 0) while
     * holding that same lock. So no worker can be parked while a submitted
     * task is still pending -- a worker that misses the signal finds the
     * work on its next loop instead of sleeping through it. */
    fr_cond_signal(sched->idle_cond);
    return 0;
}

size_t fr_sched_pool_queued(fr_scheduler_t *sched) {
    if (!sched) return 0;
    size_t total = 0;
    for (int i = 0; i < sched->worker_count; i++) {
        total += fr_native_queue_count(&sched->native_queues[i]);
    }
    return total;
}

typedef struct {
    fr_sched_native_fn1_t fn;
    int64_t arg;
} sched_native_arg1_t;

typedef struct {
    fr_sched_native_fn2_t fn;
    int64_t id;
    int64_t total;
    fr_mutex_t *lock;
    int *remaining;
    fr_cond_t *done;
} sched_indexed_ctx_t;

static void sched_native_trampoline1(void *p) {
    sched_native_arg1_t *ctx = (sched_native_arg1_t *)p;
    fr_sched_native_fn1_t fn = ctx->fn;
    int64_t arg = ctx->arg;
    free(ctx);
    fn(arg);
}

static void sched_native_trampoline2(void *p) {
    sched_indexed_ctx_t *ctx = (sched_indexed_ctx_t *)p;
    ctx->fn(ctx->id, ctx->total);
    fr_mutex_lock(ctx->lock);
    (*ctx->remaining)--;
    if (*ctx->remaining == 0) fr_cond_broadcast(ctx->done);
    fr_mutex_unlock(ctx->lock);
    free(ctx);
}

int64_t fr_sched_pool_spawn(fr_sched_native_fn1_t fn, int64_t arg) {
    fr_scheduler_t *sched = g_global_sched;
    if (!sched || !sched->running || !fn) return -1;
    sched_native_arg1_t *ctx = (sched_native_arg1_t *)malloc(sizeof(sched_native_arg1_t));
    if (!ctx) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    if (fr_sched_pool_submit(sched, sched_native_trampoline1, ctx) != 0) {
        free(ctx);
        return -1;
    }
    return 0;
}

void fr_sched_pool_spawn_indexed(fr_sched_native_fn2_t fn, int64_t count) {
    fr_scheduler_t *sched = g_global_sched;
    if (!sched || !sched->running || !fn || count <= 0) return;
    if (count > sched->worker_count * 64) count = sched->worker_count * 64;

    fr_mutex_t *lock = fr_mutex_create();
    fr_cond_t *done = fr_cond_create();
    int remaining = (int)count;
    if (!lock || !done) {
        fr_mutex_destroy(lock);
        fr_cond_destroy(done);
        return;
    }

    int64_t launched = 0;
    for (int64_t i = 0; i < count; i++) {
        sched_indexed_ctx_t *ctx = (sched_indexed_ctx_t *)calloc(1, sizeof(sched_indexed_ctx_t));
        if (!ctx) break;
        ctx->fn = fn;
        ctx->id = i;
        ctx->total = count;
        ctx->lock = lock;
        ctx->remaining = &remaining;
        ctx->done = done;
        if (fr_sched_pool_submit(sched, sched_native_trampoline2, ctx) != 0) {
            free(ctx);
            break;
        }
        launched++;
    }

    fr_mutex_lock(lock);
    /* Only the tasks that actually reached a queue will ever run their
     * trampoline and decrement `remaining`; without discounting the rest,
     * a single failed submission parks this thread here forever. */
    remaining -= (int)(count - launched);
    while (remaining > 0) fr_cond_wait(done, lock);
    fr_mutex_unlock(lock);
    fr_mutex_destroy(lock);
    fr_cond_destroy(done);
}

void fr_scheduler_add_process(fr_scheduler_t *sched, fr_process_t *proc) {
    static atomic_int next_worker;
    proc->sched = sched;
    proc->worker_id = atomic_fetch_add_explicit(&next_worker, 1, memory_order_relaxed) % sched->worker_count;
    fr_mutex_lock(sched->lock);
    proc->next = sched->processes;
    sched->processes = proc;
    fr_mutex_unlock(sched->lock);
}

fr_event_loop_t *fr_scheduler_event_loop(fr_scheduler_t *sched) {
    return sched ? sched->event_loop : NULL;
}

int fr_scheduler_worker_count(fr_scheduler_t *sched) {
    return sched ? sched->worker_count : 0;
}

void fr_scheduler_run(fr_scheduler_t *sched) {
    if (!sched) return;
    fr_scheduler_start(sched);
    /* sched->lock protects per-operation state (process list, native_pending,
     * etc.) and must not be held across this entire polling loop -- doing so
     * previously starved any concurrent fr_scheduler_add_process/thread.spawn
     * caller (and anything else taking sched->lock) for the whole program
     * lifetime, since this loop only returns when the scheduler is done.
     * sched->done is only ever touched by this thread, so no lock is needed
     * to read/write it here. */
    while (!sched->done) {
        fr_event_loop_poll(sched->event_loop, 1);
        if (!sched_has_work(sched)) sched->done = 1;
    }
    fr_scheduler_stop(sched);
}

/* ---------------------------------------------------------------------- *
 * Processes
 * ---------------------------------------------------------------------- */

fr_process_t *fr_process_create(const char *name) {
    fr_process_t *p = (fr_process_t *)calloc(1, sizeof(fr_process_t));
    if (!p) return NULL;
    p->name = fr_strdup(name ? name : "process");
    p->next_coro_id = 1;
    p->lock = fr_mutex_create();
    p->msg_cond = fr_cond_create();
    return p;
}

void fr_process_destroy(fr_process_t *proc) {
    if (!proc) return;
    fr_coro_t *c = proc->coros;
    while (c) {
        fr_coro_t *next = c->next;
        free(c->state);
        fr_arena_destroy(c->arena);
        free(c);
        c = next;
    }
    for (size_t i = 0; i < proc->mailbox.count; i++) {
        fr_msg_t *m = &proc->mailbox.msgs[(proc->mailbox.head + i) % MAILBOX_CAP];
        if (m->owns_payload && m->payload) free(m->payload);
    }
    fr_mutex_destroy(proc->lock);
    fr_cond_destroy(proc->msg_cond);
    free(proc->children);
    free(proc->name);
    free(proc);
}

fr_scheduler_t *fr_process_scheduler(fr_process_t *proc) {
    return proc->sched;
}

const char *fr_process_name(fr_process_t *proc) {
    return proc ? proc->name : "";
}

void fr_process_set_receive_handler(fr_process_t *proc, fr_coro_fn handler, size_t state_size) {
    proc->receive_handler = handler;
    proc->receive_state_size = state_size;
}

/* Takes ownership of init_state (freed here, on every path). */
fr_coro_t *fr_coro_spawn(fr_process_t *proc, fr_coro_fn fn, void *init_state, size_t state_size) {
    fr_coro_t *c = (fr_coro_t *)calloc(1, sizeof(fr_coro_t));
    if (!c) {
        free(init_state);
        return NULL;
    }
    c->state = malloc(state_size);
    if (!c->state) {
        free(c);
        free(init_state);
        return NULL;
    }
    memcpy(c->state, init_state, state_size);
    free(init_state);
    c->fn = fn;
    c->state_size = state_size;
    c->status = FR_CORO_RUNNING;
    c->step = 0;
    c->proc = proc;
    c->await_fd = -1;

    /* proc->coros is walked under proc->lock by process_has_active() and
     * scan_enqueue_runnable() running on other threads, and a coroutine can
     * spawn another from whichever worker it happens to be on -- so the
     * append and the id counter belong under the same lock the readers use.
     * enqueue_coro stays outside it: the coroutine must be fully linked and
     * initialised before another worker can pick it up. */
    fr_mutex_lock(proc->lock);
    c->id = proc->next_coro_id++;
    if (!proc->coros) proc->coros = proc->coro_tail = c;
    else { proc->coro_tail->next = c; proc->coro_tail = c; }
    fr_mutex_unlock(proc->lock);

    if (proc->sched) enqueue_coro(proc->sched, c);
    return c;
}

fr_coro_status_t fr_yield(fr_coro_t *coro) {
    coro->status = FR_CORO_YIELDED;
    return FR_CORO_YIELDED;
}

int fr_coro_id(fr_coro_t *coro) {
    return coro ? coro->id : -1;
}

fr_process_t *fr_coro_process(fr_coro_t *coro) {
    return coro ? coro->proc : NULL;
}

int fr_coro_step(fr_coro_t *coro) {
    return coro ? coro->step : 0;
}

void fr_coro_set_step(fr_coro_t *coro, int step) {
    if (coro) coro->step = step;
}

/* Takes ownership of payload: the queued message carries owns_payload, and
 * the receiver frees it via fr_msg_free_payload. Every path that fails to
 * hand it over therefore has to free it here rather than leak it. */
void fr_send(fr_process_t *dst, int tag, int64_t value, void *payload, size_t payload_size) {
    if (!dst) {
        free(payload);
        return;
    }
    fr_msg_t msg = { tag, value, payload, payload_size, payload ? 1 : 0, NULL };
    fr_mutex_lock(dst->lock);
    int queued = mailbox_push(&dst->mailbox, msg);
    if (queued) fr_cond_broadcast(dst->msg_cond);
    fr_mutex_unlock(dst->lock);
    if (!queued) {
        /* Mailbox full -- the message is dropped. */
        free(payload);
        return;
    }
    if (dst->sched) fr_cond_broadcast(dst->sched->idle_cond);
}

int fr_try_recv(fr_process_t *self, fr_msg_t *out) {
    if (!self || !out) return 0;
    fr_mutex_lock(self->lock);
    int ok = mailbox_pop(&self->mailbox, out);
    fr_mutex_unlock(self->lock);
    return ok;
}

int fr_recv(fr_process_t *self, fr_msg_t *out) {
    if (!self || !out) return 0;
    fr_mutex_lock(self->lock);
    while (!mailbox_pop(&self->mailbox, out)) {
        fr_cond_wait(self->msg_cond, self->lock);
    }
    fr_mutex_unlock(self->lock);
    return 1;
}

void fr_msg_free_payload(fr_msg_t *msg) {
    if (msg && msg->owns_payload && msg->payload) {
        free(msg->payload);
        msg->payload = NULL;
        msg->owns_payload = 0;
    }
}

int64_t fr_await_fd(fr_coro_t *coro, int64_t fd, uint32_t events) {
    if (!coro) return 1;
    if (coro->await_ready && coro->await_fd == (int)fd) {
        coro->await_ready = 0;
        coro->await_fd = -1;
        fr_event_loop_del(coro->proc->sched->event_loop, (int)fd);
        return 1;
    }
    coro->await_fd = (int)fd;
    coro->await_events = events;
    coro->await_ready = 0;
    coro->status = FR_CORO_WAITING_IO;
    fr_event_loop_add(coro->proc->sched->event_loop, (int)fd, events | FR_EVENT_ONESHOT, coro);
    return 0;
}

/* ---------------------------------------------------------------------- *
 * Supervisors
 * ---------------------------------------------------------------------- */

fr_process_t *fr_supervisor_create(const char *name, fr_restart_policy_t policy) {
    fr_process_t *p = fr_process_create(name);
    if (!p) return NULL;
    p->is_supervisor = 1;
    p->restart_policy = policy;
    return p;
}

void fr_supervisor_add_child(fr_process_t *supervisor, fr_process_t *child) {
    if (!supervisor) return;
    size_t n = supervisor->child_count + 1;
    fr_process_t **grown = (fr_process_t **)realloc(
        supervisor->children, n * sizeof(fr_process_t *));
    if (!grown) return; /* keep the existing children rather than losing them */
    grown[n - 1] = child;
    supervisor->children = grown;
    supervisor->child_count = n;
}

/* ---------------------------------------------------------------------- *
 * Event-loop bridge exposed to generated code
 * ---------------------------------------------------------------------- */

int fr_event_poll(fr_scheduler_t *sched, int timeout_ms) {
    if (!sched || !sched->event_loop) return -1;
    return fr_event_loop_poll(sched->event_loop, timeout_ms);
}

int64_t fr_event_add_read(fr_scheduler_t *sched, int64_t fd) {
    if (!sched || !sched->event_loop || fd < 0) return -1;
    if (fr_event_loop_add(sched->event_loop, (int)fd, FR_EVENT_READ, NULL) < 0) return -1;
    return fd;
}
