/* ============================================================
 * pool.h — worker 執行緒池 + work-stealing 佇列（PLAN §8.3）
 * 每個 worker 一次處理一個 batch（64 lane）。
 * ============================================================ */
#ifndef BSTM_POOL_H
#define BSTM_POOL_H

#include "bstm_types.h"
#include "batch.h"
#include "bstm_abi.h"

typedef void (*bstm_job_fn)(bstm_batch_t *b, void *ctx, int worker_id);

typedef struct bstm_pool bstm_pool_t;

bstm_pool_t *bstm_pool_create(int n_workers);
void         bstm_pool_destroy(bstm_pool_t *p);
int          bstm_pool_nworkers(const bstm_pool_t *p);

/* 把 n 個 batch 丟進 work-stealing 佇列跑完才回來。 */
int  bstm_pool_run(bstm_pool_t *p, bstm_batch_t *b, int n,
                   bstm_job_fn fn, void *ctx);

/* 統計：每個 worker 自己做了幾個、偷了幾個 */
void bstm_pool_stats(const bstm_pool_t *p, uint64_t *done, uint64_t *stolen);

/* ---- PLAN §8.4 的主迴圈：跑一個 batch 到所有 lane 結束 ---- */
typedef struct {
    uint64_t max_cycles;       /* 0 = 無限 */
    uint32_t done_check_mask;  /* 每 (mask+1) cycle 檢查一次結束，預設 1023 */
} bstm_run_opt_t;

/* 回傳跑了幾個 target cycle。counters 要有 lanes*n_counters 個 uint64。 */
uint64_t bstm_run_batch(const bstm_model_t *m, bstm_batch_t *b,
                        const bstm_run_opt_t *opt, uint64_t *counters);

#endif /* BSTM_POOL_H */
