/* ============================================================
 * stats.h — bit-sliced counter -> per-lane uint64 + CSV 輸出
 *
 * 模型內部：cnt[i][b] 是一個 vec_t，bit l = lane l 的第 b 個 bit。
 * 結束時 transpose 回 counters[lane][i]。
 * PLAN 的長期目標是 Parquet，demo 先輸出 CSV。
 * ============================================================ */
#ifndef BSTM_STATS_H
#define BSTM_STATS_H

#include "bstm_types.h"
#include "batch.h"

/* bs 是 [n_counters][width] 的 bit-sliced 陣列；
 * out 是 [BSTM_LANES][n_counters] 的 row-major。 */
void bstm_stats_extract(const vec_t *bs, int n_counters, int width,
                        uint64_t *out);

/* 反向：給測試 / 初始化用 */
void bstm_stats_inject(const uint64_t *in, int n_counters, int width, vec_t *bs);

/* CSV：表頭 config_id,simpoint_id,cfg_*,<counter names>,ipc */
int  bstm_stats_csv_header(void *fp, const char *const *cnames, int n_counters);
int  bstm_stats_csv_batch (void *fp, const bstm_batch_t *b,
                           const char *const *cnames, int n_counters,
                           const uint64_t *counters,
                           int idx_cycles, int idx_retired);

#endif /* BSTM_STATS_H */
