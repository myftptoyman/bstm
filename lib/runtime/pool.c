#define _GNU_SOURCE
#include "pool.h"
#include "refill.h"
#include "stats.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------- work-stealing deque ----------------
 * owner 從 bottom push/pop（LIFO，locality 好），thief 從 top 偷。
 * 用 per-deque mutex：steal 才會競爭，owner 的 fast path 幾乎無爭用。 */
typedef struct {
    pthread_mutex_t mu;
    int *buf;
    int  cap, top, bot;           /* [top, bot) */
} deque_t;

static void dq_init(deque_t *d, int cap)
{
    pthread_mutex_init(&d->mu, NULL);
    d->buf = malloc((size_t)cap * sizeof(int));
    d->cap = cap; d->top = d->bot = 0;
}
static void dq_fini(deque_t *d) { free(d->buf); pthread_mutex_destroy(&d->mu); }

static void dq_push(deque_t *d, int v)
{
    pthread_mutex_lock(&d->mu);
    if (d->bot == d->cap) {
        d->buf = realloc(d->buf, (size_t)d->cap * 2 * sizeof(int));
        d->cap *= 2;
    }
    d->buf[d->bot++] = v;
    pthread_mutex_unlock(&d->mu);
}
static int dq_pop(deque_t *d, int *v)      /* owner: 從 bottom */
{
    int ok = 0;
    pthread_mutex_lock(&d->mu);
    if (d->bot > d->top) { *v = d->buf[--d->bot]; ok = 1; }
    pthread_mutex_unlock(&d->mu);
    return ok;
}
static int dq_steal(deque_t *d, int *v)    /* thief: 從 top */
{
    int ok = 0;
    pthread_mutex_lock(&d->mu);
    if (d->bot > d->top) { *v = d->buf[d->top++]; ok = 1; }
    pthread_mutex_unlock(&d->mu);
    return ok;
}

struct bstm_pool {
    int          n;
    pthread_t   *th;
    deque_t     *dq;
    uint64_t    *n_done, *n_stolen;
    /* 本輪工作 */
    bstm_batch_t *batches;
    bstm_job_fn   fn;
    void         *ctx;
    pthread_mutex_t gate;
    pthread_cond_t  start_cv, done_cv;
    int  generation, n_active, running, shutdown;
};

typedef struct { bstm_pool_t *p; int id; } warg_t;

static void *worker_main(void *a)
{
    warg_t *w = a;
    bstm_pool_t *p = w->p;
    int id = w->id, mygen = 0;
    free(a);
    for (;;) {
        pthread_mutex_lock(&p->gate);
        while (!p->shutdown && p->generation == mygen)
            pthread_cond_wait(&p->start_cv, &p->gate);
        if (p->shutdown) { pthread_mutex_unlock(&p->gate); break; }
        mygen = p->generation;
        pthread_mutex_unlock(&p->gate);

        for (;;) {
            int idx;
            if (dq_pop(&p->dq[id], &idx)) {
                p->fn(&p->batches[idx], p->ctx, id);
                p->n_done[id]++;
                continue;
            }
            {   /* 偷 */
                int k, got = 0;
                for (k = 1; k < p->n; k++) {
                    int v = (id + k) % p->n;
                    if (dq_steal(&p->dq[v], &idx)) {
                        p->fn(&p->batches[idx], p->ctx, id);
                        p->n_done[id]++; p->n_stolen[id]++;
                        got = 1; break;
                    }
                }
                if (!got) break;
            }
        }

        pthread_mutex_lock(&p->gate);
        if (--p->n_active == 0) pthread_cond_signal(&p->done_cv);
        pthread_mutex_unlock(&p->gate);
    }
    return NULL;
}

bstm_pool_t *bstm_pool_create(int n)
{
    bstm_pool_t *p;
    int i;
    if (n <= 0) n = 1;
    p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->n        = n;
    p->th       = calloc((size_t)n, sizeof(pthread_t));
    p->dq       = calloc((size_t)n, sizeof(deque_t));
    p->n_done   = calloc((size_t)n, sizeof(uint64_t));
    p->n_stolen = calloc((size_t)n, sizeof(uint64_t));
    pthread_mutex_init(&p->gate, NULL);
    pthread_cond_init(&p->start_cv, NULL);
    pthread_cond_init(&p->done_cv, NULL);
    for (i = 0; i < n; i++) dq_init(&p->dq[i], 64);
    for (i = 0; i < n; i++) {
        warg_t *a = malloc(sizeof *a);
        a->p = p; a->id = i;
        pthread_create(&p->th[i], NULL, worker_main, a);
    }
    return p;
}

void bstm_pool_destroy(bstm_pool_t *p)
{
    int i;
    if (!p) return;
    pthread_mutex_lock(&p->gate);
    p->shutdown = 1;
    pthread_cond_broadcast(&p->start_cv);
    pthread_mutex_unlock(&p->gate);
    for (i = 0; i < p->n; i++) pthread_join(p->th[i], NULL);
    for (i = 0; i < p->n; i++) dq_fini(&p->dq[i]);
    free(p->th); free(p->dq); free(p->n_done); free(p->n_stolen);
    pthread_mutex_destroy(&p->gate);
    pthread_cond_destroy(&p->start_cv);
    pthread_cond_destroy(&p->done_cv);
    free(p);
}

int bstm_pool_nworkers(const bstm_pool_t *p) { return p ? p->n : 0; }

int bstm_pool_run(bstm_pool_t *p, bstm_batch_t *b, int n, bstm_job_fn fn, void *ctx)
{
    int i;
    if (!p || n <= 0) return 0;
    p->batches = b; p->fn = fn; p->ctx = ctx;
    for (i = 0; i < n; i++) dq_push(&p->dq[i % p->n], i);
    pthread_mutex_lock(&p->gate);
    p->n_active = p->n;
    p->generation++;
    pthread_cond_broadcast(&p->start_cv);
    while (p->n_active > 0) pthread_cond_wait(&p->done_cv, &p->gate);
    pthread_mutex_unlock(&p->gate);
    return 0;
}

void bstm_pool_stats(const bstm_pool_t *p, uint64_t *done, uint64_t *stolen)
{
    int i;
    for (i = 0; i < p->n; i++) { done[i] = p->n_done[i]; stolen[i] = p->n_stolen[i]; }
}

/* ---------------- PLAN §8.4 主迴圈 ---------------- */
uint64_t bstm_run_batch(const bstm_model_t *m, bstm_batch_t *b,
                        const bstm_run_opt_t *opt, uint64_t *counters)
{
    const bstm_model_desc_t *d = m->describe();
    bstm_refill_t *rf = malloc(sizeof *rf);
    bstm_fbwin_t  *win = malloc(sizeof *win);
    bstm_out_t     out;
    void *cur = m->state_alloc(), *nxt = m->state_alloc(), *tmp;
    uint64_t cyc = 0, done = 0, all = b->lane_mask;
    uint32_t chk = opt && opt->done_check_mask ? opt->done_check_mask : 1023u;

    bstm_refill_init(rf, b->trace_of, b->lane_mask);
    m->init(cur, b->cfg, b->lane_mask);
    m->init(nxt, b->cfg, b->lane_mask);

    for (;;) {
        bstm_refill_window(rf, win);
        m->eval(nxt, cur, win, &out);
        /* 已結束的 lane 不再消耗 trace、也不再累加 counter */
        out.take[0] &= ~done; out.take[1] &= ~done; out.take[2] &= ~done;
        out.redirect &= ~done;
        bstm_refill_consume(rf, out.take, out.redirect, out.redir_shadow);
        tmp = cur; cur = nxt; nxt = tmp;
        cyc++;

        if ((cyc & chk) == 0) {
            done |= bstm_refill_done_mask(rf);
            if (m->done_mask) done |= m->done_mask(cur);
            if (m->set_enable) m->set_enable(cur, all & ~done);
            if ((done & all) == all) break;
        }
        if (opt && opt->max_cycles && cyc >= opt->max_cycles) break;
    }

    m->extract(cur, counters);
    b->target_cycles = cyc;

    m->state_free(cur); m->state_free(nxt);
    free(rf); free(win);
    (void)d;
    return cyc;
}
