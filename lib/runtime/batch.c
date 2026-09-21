#include "batch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const bstm_cfg_names[BSTM_CFG_N] = {
    "cfg_rob_entries", "cfg_iq_entries", "cfg_ldq_entries", "cfg_stq_entries",
    "cfg_mshr_entries", "cfg_fetch_width", "cfg_issue_width", "cfg_commit_width"
};
const uint8_t bstm_cfg_widths[BSTM_CFG_N] = { 7, 6, 5, 5, 4, 3, 3, 3 };

static int cmp_inst(const void *a, const void *b)
{
    const bstm_instance_t *x = a, *y = b;
    if (x->simpoint_id != y->simpoint_id)
        return x->simpoint_id < y->simpoint_id ? -1 : 1;
    if (x->config_id != y->config_id)
        return x->config_id < y->config_id ? -1 : 1;
    return 0;
}

int bstm_batch_plan(const bstm_instance_t *inst, int n_inst,
                    const bstf_trace_t *const *traces, int n_traces,
                    bstm_batchset_t *out)
{
    bstm_instance_t *s;
    int i, nb = 0, cap;
    memset(out, 0, sizeof *out);
    if (n_inst <= 0) return 0;

    s = malloc((size_t)n_inst * sizeof *s);
    if (!s) return -1;
    memcpy(s, inst, (size_t)n_inst * sizeof *s);
    /* 同 simpoint 的 instance 排在一起 -> 自然形成共用 trace 的 batch */
    qsort(s, (size_t)n_inst, sizeof *s, cmp_inst);

    cap = (n_inst + BSTM_LANES - 1) / BSTM_LANES + 8;
    out->b = calloc((size_t)cap, sizeof *out->b);
    if (!out->b) { free(s); return -1; }

    i = 0;
    while (i < n_inst) {
        bstm_batch_t *B = &out->b[nb];
        uint32_t sp = s[i].simpoint_id;
        int l = 0;
        B->simpoint_id = sp;
        B->trace = (sp < (uint32_t)n_traces) ? traces[sp] : NULL;
        while (i < n_inst && s[i].simpoint_id == sp && l < BSTM_LANES) {
            int k;
            B->inst[l]     = s[i];
            B->lane_mask  |= (uint64_t)1 << l;
            B->trace_of[l] = B->trace;
            for (k = 0; k < BSTM_CFG_N; k++)
                B->cfg[l * BSTM_CFG_N + k] = s[i].cfg[k];
            l++; i++;
        }
        B->n_lanes = l;                     /* 其餘 lane 保持空 */
        nb++;
        if (nb == cap) {                    /* 理論上不會，保險 */
            bstm_batch_t *nbuf = realloc(out->b, (size_t)(cap * 2) * sizeof *out->b);
            if (!nbuf) { free(s); free(out->b); out->b = NULL; return -1; }
            memset(nbuf + cap, 0, (size_t)cap * sizeof *nbuf);
            out->b = nbuf; cap *= 2;
        }
    }
    out->n = nb;
    free(s);
    return 0;
}

void bstm_batchset_free(bstm_batchset_t *bs)
{
    int i;
    if (!bs->b) return;
    for (i = 0; i < bs->n; i++) free(bs->b[i].counters);
    free(bs->b);
    bs->b = NULL; bs->n = 0;
}

void bstm_batch_dump(const bstm_batchset_t *bs, void *fpv)
{
    FILE *fp = fpv ? fpv : stdout;
    int i;
    for (i = 0; i < bs->n; i++) {
        const bstm_batch_t *B = &bs->b[i];
        fprintf(fp, "batch %3d  simpoint %u  lanes %2d/%d  empty %2d\n",
                i, B->simpoint_id, B->n_lanes, BSTM_LANES, BSTM_LANES - B->n_lanes);
    }
}
