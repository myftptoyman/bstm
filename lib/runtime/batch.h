/* ============================================================
 * batch.h — instance 打包（PLAN §8.1 / §8.2）
 *
 * instance = (config_id, simpoint_id)
 * 打包策略：優先「同一個 simpoint、不同 config」放同一 batch，
 *           因為它們共用同一份 trace，L2 命中率最高。
 * config 數不足 64 時允許空 lane。
 * ============================================================ */
#ifndef BSTM_BATCH_H
#define BSTM_BATCH_H

#include "bstm_types.h"
#include "bstf_reader.h"
#include "bstm_abi.h"

typedef struct {
    uint32_t config_id;
    uint32_t simpoint_id;
    uint32_t cfg[BSTM_CFG_N];
} bstm_instance_t;

typedef struct {
    uint32_t simpoint_id;                  /* 這個 batch 共用的 trace */
    const bstf_trace_t *trace;
    uint64_t lane_mask;                    /* 哪些 lane 有效 */
    int      n_lanes;
    bstm_instance_t inst[BSTM_LANES];      /* lane -> instance */
    uint32_t cfg[BSTM_LANES * BSTM_CFG_N]; /* 給 model->init 的 row-major */
    const bstf_trace_t *trace_of[BSTM_LANES];
    /* 執行結果 */
    uint64_t *counters;                    /* [lane][n_counters]，由 pool 配置 */
    uint64_t  target_cycles;
} bstm_batch_t;

typedef struct {
    bstm_batch_t *b;
    int           n;
} bstm_batchset_t;

/* 依 §8.2 打包。traces[simpoint_id] 給出每個 simpoint 的 trace。
 * 回傳 0 成功；batchset 用 bstm_batchset_free 釋放。 */
int  bstm_batch_plan(const bstm_instance_t *inst, int n_inst,
                     const bstf_trace_t *const *traces, int n_traces,
                     bstm_batchset_t *out);
void bstm_batchset_free(bstm_batchset_t *bs);

/* 除錯用：印出打包結果 */
void bstm_batch_dump(const bstm_batchset_t *bs, void *fp);

#endif /* BSTM_BATCH_H */
