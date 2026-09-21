#include "stats.h"
#include "transpose.h"
#include <stdio.h>
#include <string.h>

void bstm_stats_extract(const vec_t *bs, int n_counters, int width, uint64_t *out)
{
    uint64_t A[64];
    int i, b, l;
    for (i = 0; i < n_counters; i++) {
        for (b = 0; b < width && b < 64; b++) A[b] = (uint64_t)bs[(size_t)i * width + b];
        for (; b < 64; b++)                   A[b] = 0;
        bstm_transpose64(A);                  /* A[l] = lane l 的值 */
        for (l = 0; l < 64; l++) out[(size_t)l * n_counters + i] = A[l];
    }
}

void bstm_stats_inject(const uint64_t *in, int n_counters, int width, vec_t *bs)
{
    uint64_t A[64];
    int i, b, l;
    for (i = 0; i < n_counters; i++) {
        for (l = 0; l < 64; l++) A[l] = in[(size_t)l * n_counters + i];
        bstm_transpose64(A);
        for (b = 0; b < width && b < 64; b++) bs[(size_t)i * width + b] = (vec_t)A[b];
    }
}

int bstm_stats_csv_header(void *fpv, const char *const *cnames, int n)
{
    FILE *fp = fpv;
    int i;
    fprintf(fp, "config_id,simpoint_id,lane");
    for (i = 0; i < BSTM_CFG_N; i++) fprintf(fp, ",%s", bstm_cfg_names[i]);
    for (i = 0; i < n; i++)          fprintf(fp, ",%s", cnames[i]);
    fprintf(fp, ",ipc\n");
    return 0;
}

int bstm_stats_csv_batch(void *fpv, const bstm_batch_t *b,
                         const char *const *cnames, int n,
                         const uint64_t *counters, int idx_cycles, int idx_retired)
{
    FILE *fp = fpv;
    int l, i;
    (void)cnames;
    for (l = 0; l < BSTM_LANES; l++) {
        const uint64_t *c = counters + (size_t)l * n;
        double ipc;
        if (!((b->lane_mask >> l) & 1)) continue;
        fprintf(fp, "%u,%u,%d", b->inst[l].config_id, b->inst[l].simpoint_id, l);
        for (i = 0; i < BSTM_CFG_N; i++) fprintf(fp, ",%u", b->inst[l].cfg[i]);
        for (i = 0; i < n; i++)          fprintf(fp, ",%llu", (unsigned long long)c[i]);
        ipc = (idx_cycles >= 0 && idx_retired >= 0 && c[idx_cycles])
            ? (double)c[idx_retired] / (double)c[idx_cycles] : 0.0;
        fprintf(fp, ",%.6f\n", ipc);
    }
    return 0;
}
