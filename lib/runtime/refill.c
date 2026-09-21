#include "refill.h"
#include "transpose.h"
#include <string.h>

/* 把游標處的記錄解碼成 entry：bit47..0 = duop|fe|mem，bit48 = 該記錄有 shadow */
static uint64_t decode_entry(const bstf_trace_t *t, const bstf_cursor_t *c,
                             const bstf_rec_t *rec)
{
    uint64_t e = bstf_rec_to_duop(rec, c->in_shadow);
    e |= (uint64_t)bstf_fe_event (t, c) << 32;
    e |= (uint64_t)bstf_mem_event_of(t, c, rec) << 40;   /* 非 mem 指令 -> 0 */
    if (!c->in_shadow && rec->shadow_off != 0 && rec->shadow_len != 0)
        e |= (uint64_t)1 << 48;
    return e;
}

void bstm_refill_init(bstm_refill_t *r,
                      const bstf_trace_t *const traces[BSTM_LANES],
                      uint64_t lane_mask)
{
    int l;
    memset(r, 0, sizeof *r);
    r->lane_mask = lane_mask;
    for (l = 0; l < BSTM_LANES; l++) {
        bstm_lane_t *L = &r->lane[l];
        if (!((lane_mask >> l) & 1) || !traces[l]) { L->active = 0; L->eof = 1; continue; }
        L->trace  = traces[l];
        L->active = 1;
        bstf_cur_init(L->trace, &L->cur);
        bstm_refill_gather(r, l);
    }
}

/* PLAN §8.5 步驟 2-3：scalar gather。這是唯一會碰到 mmap 與分支的地方。 */
void bstm_refill_gather(bstm_refill_t *r, int l)
{
    bstm_lane_t *L = &r->lane[l];
    if (!L->active) return;

    if (L->head) {                       /* 壓縮到陣列開頭 */
        if (L->cnt) {
            memmove(L->stage, L->stage + L->head, L->cnt * sizeof L->stage[0]);
            memmove(L->pos,   L->pos   + L->head, L->cnt * sizeof L->pos[0]);
        }
        L->head = 0;
    }
    r->n_gather++;

    while (L->cnt < BSTM_REFILL_M) {
        const bstf_rec_t *rec = bstf_peek(L->trace, &L->cur);
        if (!rec) {
            if (L->cur.in_shadow) {      /* shadow 讀完自己回正確路徑 */
                bstf_leave_shadow(L->trace, &L->cur);
                continue;
            }
            L->eof = 1;
            break;
        }
        L->pos[L->cnt].rec_byte = L->cur.in_shadow ? 0 : L->cur.rec_byte;
        L->pos[L->cnt].fe_idx   = L->cur.fe_idx;
        L->pos[L->cnt].mem_idx  = L->cur.mem_idx;
        L->stage[L->cnt]        = decode_entry(L->trace, &L->cur, rec);
        L->cnt++;
        bstf_advance(L->trace, &L->cur, 1);
        r->n_gathered++;
    }
}

/* PLAN §8.5 步驟 4：W 筆 window -> 3 個 word -> 3 次 64x64 轉置 */
void bstm_refill_window(bstm_refill_t *r, bstm_fbwin_t *win)
{
    /* 三個 word 各用一個連續陣列，轉置可以直接原地做，省掉 stride gather */
    uint64_t P0[64], P1[64], P2[64];
    int l, b;

    r->n_cycles++;

    for (l = 0; l < BSTM_LANES; l++) {
        bstm_lane_t *L = &r->lane[l];
        uint64_t w0 = 0, w1 = 0, w2 = 0;
        if (L->active && L->cnt < BSTM_W && !L->eof)
            bstm_refill_gather(r, l);
        {   /* 手動展開 W=4：每 cycle 每 lane 都要跑，分支越少越好 */
            const uint64_t *st = L->stage + L->head;
            unsigned n = L->cnt < BSTM_W ? L->cnt : BSTM_W;
            uint64_t e0 = n > 0 ? st[0] : 0, e1 = n > 1 ? st[1] : 0;
            uint64_t e2 = n > 2 ? st[2] : 0, e3 = n > 3 ? st[3] : 0;
            w0 = (e0 & 0xFFFFFFFFu) | ((e1 & 0xFFFFFFFFu) << 32);
            w1 = (e2 & 0xFFFFFFFFu) | ((e3 & 0xFFFFFFFFu) << 32);
            w2 = ((e0 >> 40) & 0xFFu)
               | (((e1 >> 40) & 0xFFu) << 8)
               | (((e2 >> 40) & 0xFFu) << 16)
               | (((e3 >> 40) & 0xFFu) << 24)
               | (((e0 >> 32) & 0xFFu) << 32)                 /* fb_fe_event */
               | ((uint64_t)(((uint64_t)1 << n) - 1) << 40);   /* fb_valid    */
        }
        P0[l] = w0; P1[l] = w1; P2[l] = w2;
    }

    bstm_transpose64(P0);
    bstm_transpose64(P1);
    bstm_transpose64(P2);
    for (b = 0; b < 64; b++) { win->duop[b] = P0[b]; win->duop[64 + b] = P1[b]; }
    for (b = 0; b < BSTM_W * BSTM_MEM_W; b++) win->mem[b]   = P2[b];
    for (b = 0; b < BSTM_FE_W;           b++) win->fe[b]    = P2[32 + b];
    for (b = 0; b < BSTM_W;              b++) win->valid[b] = P2[40 + b];
}


/* redirect_shadow 的語意：跳到「游標當下或之後第一個帶 shadow 的記錄」的
 * wrong-path，解析後從該記錄的下一筆回到正確路徑。
 *
 * 為什麼是「之後」而不是「最近一筆已消耗的」：fe_event 是 per-fetch-block 的，
 * 模型看到 FE_REDIRECT 時，觸發的分支通常是該 block 的最後一條，還沒被消耗。
 * 若往回找，正確路徑游標會倒退，可能永遠不前進（實測會卡死）。往前找可以
 * 保證每次 redirect 至少跨過一個分支，correct-path 單調前進。
 * 這個約定 top.v 沒有規定 —— 已回報監督者。 */
static void lane_enter_shadow(bstm_refill_t *r, bstm_lane_t *L)
{
    int i;
    if (L->cur.in_shadow) return;               /* 已經在 shadow 裡 */

    /* 1. 先找還在 staging 裡、尚未消耗的記錄。
     *    找到就把 staging 截到「含分支那一筆」為止（分支之前的正確路徑
     *    指令不該被丟掉，只有分支之後的才是被 squash 的推測 fetch）。 */
    for (i = 0; i < (int)L->cnt; i++) {
        if (!BSTM_ENT_HASSH(L->stage[L->head + i])) continue;
        memset(&L->last_br, 0, sizeof L->last_br);
        L->last_br.rec_byte = L->pos[L->head + i].rec_byte;
        L->last_br.fe_idx   = L->pos[L->head + i].fe_idx;
        L->last_br.mem_idx  = L->pos[L->head + i].mem_idx;
        L->has_last_br = 1;
        L->cur = L->last_br;
        if (bstf_enter_shadow(L->trace, &L->cur)) {
            L->cnt = (uint16_t)(i + 1);
            return;
        }
        break;
    }
    /* 2. staging 用完就往前掃（上限 4*REFILL_M 筆，避免長掃描） */
    for (i = 0; i < 4 * BSTM_REFILL_M; i++) {
        const bstf_rec_t *rec = bstf_peek(L->trace, &L->cur);
        if (!rec) return;
        if (rec->shadow_off && rec->shadow_len) {
            L->last_br = L->cur; L->has_last_br = 1;
            if (bstf_enter_shadow(L->trace, &L->cur)) return;
        }
        bstf_advance(L->trace, &L->cur, 1);
        r->n_gathered++;
    }
}

void bstm_refill_consume(bstm_refill_t *r, const vec_t take[3],
                         vec_t redirect, vec_t redir_shadow)
{
    uint8_t tk[BSTM_LANES];
    int l, i;

    bstm_unpack_lanes_small(take, tk, 3);

    for (l = 0; l < BSTM_LANES; l++) {
        bstm_lane_t *L = &r->lane[l];
        unsigned n;
        if (!L->active) continue;
        n = tk[l];
        if (n > L->cnt) n = L->cnt;

        for (i = 0; i < (int)n; i++) {
            uint64_t ent = L->stage[L->head + i];
            if (BSTM_ENT_DUOP(ent) >> 31) L->wrongpath++;
        }
        L->head    += (uint16_t)n;
        L->cnt     -= (uint16_t)n;
        L->consumed += n;

        if ((redirect >> l) & 1) {
            r->n_redirect++;
            if ((redir_shadow >> l) & 1) {
                lane_enter_shadow(r, L);   /* 自己決定 staging 保留到哪 */
            } else {
                if (L->cur.in_shadow) bstf_leave_shadow(L->trace, &L->cur);
                L->head = 0; L->cnt = 0;   /* 丟掉還沒消耗的 wrong-path */
            }
            L->eof = 0;
            bstm_refill_gather(r, l);
        }
    }
}

uint64_t bstm_refill_done_mask(const bstm_refill_t *r)
{
    uint64_t m = 0;
    int l;
    for (l = 0; l < BSTM_LANES; l++) {
        const bstm_lane_t *L = &r->lane[l];
        if (!L->active || (L->eof && L->cnt == 0)) m |= (uint64_t)1 << l;
    }
    return m;
}
