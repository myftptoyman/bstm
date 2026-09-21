/* ============================================================
 * refill.h — scalar/vector 邊界（PLAN §8.5）
 *
 * 每 target cycle 要做的事：
 *   1. 從 bit-sliced 取出每 lane 的 fb_take / fb_redirect（小 n 特化 unpack）
 *   2. scalar 側推進 cursor、處理 wrong-path 切換
 *   3. 需要時做 scalar gather：每 lane 一次抓 REFILL_M 筆進 staging
 *      （這一步才是貴的：mmap 隨機存取 + 欄位解碼 + shadow 分支）
 *   4. 把 staging 最前面 W 筆 bit-transpose 成 bit-sliced fetch window
 *
 * 實作註記：這裡**不做** bit-sliced 的 barrel shift。window 的「滑動」
 * 完全在 scalar 側（每 lane 一個 staging 索引），bit-sliced 側每 cycle
 * 重建 W 筆的 window。兩種做法的每-cycle 成本相同（都正比於 W*ENTRY_W
 * 個 bit），但重建法沒有 per-lane 變動位置寫入的問題，而且程式碼短很多。
 *
 * 每 lane 的 W 筆 window 打包成 3 個 64-bit word（= 3 次 64x64 轉置）：
 *   w0: [31:0]=duop0 [63:32]=duop1
 *   w1: [31:0]=duop2 [63:32]=duop3
 *   w2: [7:0]=mem0 [15:8]=mem1 [23:16]=mem2 [31:24]=mem3
 *       [39:32]=fe_event  [43:40]=valid[3:0]
 * ============================================================ */
#ifndef BSTM_REFILL_H
#define BSTM_REFILL_H

#include "bstm_types.h"
#include "bstf_reader.h"

#ifndef BSTM_REFILL_M
#define BSTM_REFILL_M 32          /* scalar staging 深度，可配置 */
#endif
#if BSTM_REFILL_M < (2*BSTM_W)
#  error "BSTM_REFILL_M must be >= 2*BSTM_W"
#endif

#define BSTM_FB_WORDS 3           /* 每 lane 的 window 打包成幾個 64-bit word */

/* 餵給模型的 bit-sliced fetch window，欄位對齊 model/ooo/top.v */
typedef struct {
    vec_t valid[BSTM_W];                    /* fb_valid       */
    vec_t duop [BSTM_W * BSTM_DUOP_W];      /* fb_duop        */
    vec_t fe   [BSTM_FE_W];                 /* fb_fe_event    */
    vec_t mem  [BSTM_W * BSTM_MEM_W];       /* fb_mem_event   */
} bstm_fbwin_t;

/* staging 裡每一筆記錄的「它從哪裡來」，供 redirect 反推 shadow 用 */
typedef struct { uint64_t rec_byte, fe_idx, mem_idx; } bstm_pos_t;

typedef struct {
    const bstf_trace_t *trace;
    bstf_cursor_t cur;            /* 下一筆「要進 staging」的位置 */
    bstf_cursor_t last_br;        /* 最近一筆已消耗、且有 shadow 的記錄 */
    uint8_t  has_last_br;
    uint8_t  active;
    uint8_t  eof;
    uint16_t head, cnt;           /* staging 的有效區間 [head, head+cnt) */
    uint64_t   stage[BSTM_REFILL_M];  /* bit47..0 = entry, bit48 = 有 shadow */
    bstm_pos_t pos  [BSTM_REFILL_M];
    uint64_t consumed;            /* 已消耗的記錄數（含 wrong-path） */
    uint64_t wrongpath;           /* 其中 wrong-path 的筆數 */
} bstm_lane_t;

typedef struct {
    bstm_lane_t lane[BSTM_LANES];
    uint64_t lane_mask;
    /* 效能統計 */
    uint64_t n_cycles;            /* 呼叫 window 的次數        */
    uint64_t n_gather;            /* scalar gather 呼叫次數     */
    uint64_t n_gathered;          /* gather 到的記錄總數        */
    uint64_t n_redirect;          /* 處理過的 redirect 次數     */
} bstm_refill_t;

void bstm_refill_init(bstm_refill_t *r,
                      const bstf_trace_t *const traces[BSTM_LANES],
                      uint64_t lane_mask);

/* PLAN §8.5 步驟 2-3：把單一 lane 的 staging 補滿到 REFILL_M 筆。 */
void bstm_refill_gather(bstm_refill_t *r, int l);

/* PLAN §8.5 步驟 1+4：補齊 staging 並產生這個 cycle 的 bit-sliced window。 */
void bstm_refill_window(bstm_refill_t *r, bstm_fbwin_t *win);

/* 消耗：take 是 3-bit bit-sliced 的 fb_take；redirect / redir_shadow 各 1 word。
 * redirect 語意（本 runtime 的約定，top.v 未規定）：
 *   redir_shadow=1 → 丟掉 staging，跳到「最近一筆已消耗且有 shadow 的記錄」的
 *                    shadow 區；
 *   redir_shadow=0 → 丟掉 staging，回到正確路徑的續行點。
 */
void bstm_refill_consume(bstm_refill_t *r, const vec_t take[3],
                         vec_t redirect, vec_t redir_shadow);

/* 哪些 lane 的 trace 已經跑完（bit l = lane l） */
uint64_t bstm_refill_done_mask(const bstm_refill_t *r);

/* entry 打包/解包（給測試用） */
#define BSTM_ENT_DUOP(e)  ((uint32_t)((e) & 0xFFFFFFFFu))
#define BSTM_ENT_FE(e)    ((uint8_t)(((e) >> 32) & 0xFFu))
#define BSTM_ENT_MEM(e)   ((uint8_t)(((e) >> 40) & 0xFFu))
#define BSTM_ENT_HASSH(e) ((int)(((e) >> 48) & 1u))

#endif /* BSTM_REFILL_H */
