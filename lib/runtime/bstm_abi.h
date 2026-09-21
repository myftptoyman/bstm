/* ============================================================
 * bstm_abi.h — runtime <-> 編譯出來的 bit-sliced 模型 的介面
 *
 * 對應 PLAN §13.2「執行期 ABI」，但把 fetch 介面具體化成
 * model/ooo/top.v 的 fb_* 埠（PLAN 當時還沒定下來）。
 *
 * 一份模型（.so 或靜態連結）要提供一個 `bstm_model_t`。
 * ============================================================ */
#ifndef BSTM_ABI_H
#define BSTM_ABI_H

#include "bstm_types.h"
#include "refill.h"

/* CONTRACT §3 的 cfg_* 順序，模型與 runtime 都用這個索引 */
enum {
    BSTM_CFG_ROB = 0, BSTM_CFG_IQ, BSTM_CFG_LDQ, BSTM_CFG_STQ,
    BSTM_CFG_MSHR, BSTM_CFG_FETCH_W, BSTM_CFG_ISSUE_W, BSTM_CFG_COMMIT_W,
    BSTM_CFG_N
};
extern const char *const bstm_cfg_names[BSTM_CFG_N];
extern const uint8_t     bstm_cfg_widths[BSTM_CFG_N];

/* 模型每 cycle 回給 runtime 的東西（top.v 的 output，bit-sliced） */
typedef struct {
    vec_t take[3];          /* fb_take[2:0]      */
    vec_t redirect;         /* fb_redirect       */
    vec_t redir_shadow;     /* fb_redir_shadow   */
} bstm_out_t;

typedef struct {
    const char *name;
    uint32_t    lanes;
    uint32_t    state_bytes;
    uint32_t    n_counters;
    const char *const *counter_names;
    uint32_t    counter_width;      /* 48 */
    uint32_t    n_cfg_fields;
    const char *const *cfg_names;
    const uint8_t     *cfg_widths;
} bstm_model_desc_t;

typedef struct {
    const bstm_model_desc_t *(*describe)(void);
    void *(*state_alloc)(void);
    void  (*state_free)(void *st);
    /* cfg 是 [lane][n_cfg_fields] row-major；lane_mask 外的 lane 凍結 */
    void  (*init)(void *st, const uint32_t *cfg, uint64_t lane_mask);
    /* 一個 target cycle：next <- f(cur, win)，並吐出 out */
    void  (*eval)(void *next, const void *cur, const bstm_fbwin_t *win,
                  bstm_out_t *out);
    /* counters[lane * n_counters + i]，已經 transpose 回 scalar */
    void  (*extract)(const void *st, uint64_t *counters);
    /* 哪些 lane 已經自己判定結束（沒有就回 0） */
    uint64_t (*done_mask)(const void *st);
    /* 凍結 lane（done 之後不要再污染統計）；可為 NULL */
    void  (*set_enable)(void *st, uint64_t lane_mask);
} bstm_model_t;

#endif /* BSTM_ABI_H */
