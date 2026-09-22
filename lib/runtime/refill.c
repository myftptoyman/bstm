#include "refill.h"
#include "transpose.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 硬斷言：correct-path 游標倒退是 Agent G 先前踩過的坑（500 萬 cycle 原地
 * 打轉）。這類錯誤在統計上看起來只是「IPC 偏低」，不會自己現形，所以
 * 寧可直接中止。成本是每次 redirect 一次比較，可忽略。 */
#define BSTM_HARD_ASSERT(cond, ...)                                   \
    do { if (!(cond)) {                                               \
        fprintf(stderr, "BSTM FATAL %s:%d: " #cond "\n  ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n");          \
        abort();                                                      \
    } } while (0)

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
            if (L->fstate == BSTM_FS_SHADOW) {
                /* shadow 的 shadow_len 筆供應完，但 flush 還沒到。
                 * 停止供應（= 前端斷流），**不可**自己回正確路徑：
                 * 那會讓模型在還以為自己在推測時收到正確路徑的 uop。
                 * 游標維持在 shadow 內，等 fb_redir_shadow=0 才還原。 */
                L->fstate = BSTM_FS_STARVED;
                L->n_starve++;
            }
            if (L->fstate == BSTM_FS_CORRECT) L->eof = 1;
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
        /* 診斷：window 一拍可能跨過整個 fetch block，那個 block 的 fe_event
         * 就永遠不會被模型看到（CONTRACT §5 的已知限制 G7）。這裡量它。 */
        if (L->cnt && L->fstate == BSTM_FS_CORRECT) {
            uint64_t f = L->pos[L->head].fe_idx;
            if (!L->fe_head_valid || f != L->fe_head_prev) {
                if (L->fe_head_valid && f > L->fe_head_prev + 1)
                    L->n_blk_skipped += f - L->fe_head_prev - 1;
                L->n_blk_seen++;
                if (BSTM_ENT_FE(L->stage[L->head]) & FE_REDIRECT) L->n_redir_blk_seen++;
            }
            L->fe_head_prev = f;
            L->fe_head_valid = 1;
        }
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


/* ---------------- wrong-path 狀態機 ----------------
 *
 * CONTRACT §5【G2】：
 *   fb_redirect & fb_redir_shadow 時，跳到「當前游標位置或之後、第一筆帶
 *   shadow_off != 0 的記錄」的 shadow 區。
 *
 * 搜尋範圍**只限 staging**（尚未消耗的 REFILL_M 筆），理由有兩個：
 *   1. 正確性：staging 之外的記錄還沒被交給模型，若為了找分支而把游標往前
 *      推，那些 correct-path 指令就永遠不會被交付 —— 會靜默吃掉 trace。
 *   2. 夠用：觸發 redirect 的 fe_event 屬於 window 第 0 筆所在的 fetch block，
 *      而帶 shadow 的分支就是該 block 的最後一條（CoreMark 平均 3.2 條/block），
 *      必定落在 staging 內。
 * 找不到就不動游標（記到 n_redir_nobranch），絕不倒退。
 *
 * staging 的處理：
 *   進 shadow：截到「含分支那一筆」為止。分支之前的 correct-path 指令比分支老，
 *             不該被 squash；分支之後的才是被丟掉的推測 fetch。
 *             續行點 = 分支 + 1，所以被丟掉的那些之後會重新 fetch，不會遺失。
 *   離開    ：只砍掉尾巴的 wrong-path entry（它們是連續的後綴），保留前面的
 *             correct-path 前綴 —— 那個前綴剛好接到續行點，連續無縫。
 */
/* 真正執行切換：br = 分支的位置，resume = 還原時要接回去的位置。
 * resume 一定 >= 「已交付或仍保留在 staging 的最後一筆 correct-path 記錄 + 1」，
 * 這樣每筆 correct-path 記錄恰好被交付一次（不遺失、不重複）。 */
static int do_enter(bstm_refill_t *r, bstm_lane_t *L,
                    const bstm_pos_t *br, const bstm_pos_t *resume_after)
{
    bstf_cursor_t cur;

    /* 不變式 1：correct-path 單調前進。
     * 這是 Agent G 先前踩過的坑（500 萬 cycle 原地打轉、78% retire 是
     * wrong-path）。這類錯誤只會表現成「IPC 偏低」，不會自己現形，所以
     * 寧可直接中止。成本 = 每次 redirect 一次比較。 */
    BSTM_HARD_ASSERT(!L->has_last_br || br->rec_byte > L->hi_branch_byte,
        "correct-path 游標倒退：新分支 byte=%llu <= 歷史最大 %llu (進 shadow 第 %llu 次)",
        (unsigned long long)br->rec_byte, (unsigned long long)L->hi_branch_byte,
        (unsigned long long)L->n_enter);

    memset(&cur, 0, sizeof cur);
    cur.rec_byte = br->rec_byte;
    cur.fe_idx   = br->fe_idx;
    cur.mem_idx  = br->mem_idx;
    if (!bstf_enter_shadow(L->trace, &cur)) return 0;      /* shadow_off 壞掉 */

    /* 還原點：預設是分支 + 1（bstf_enter_shadow 算的）。若已經交付到分支之後，
     * 改用「最後交付的那一筆 + 1」，避免重複交付。 */
    if (resume_after && resume_after->rec_byte + L->trace->rec_bytes > cur.sv_rec_byte) {
        bstf_cursor_t rc;
        memset(&rc, 0, sizeof rc);
        rc.rec_byte = resume_after->rec_byte;
        rc.fe_idx   = resume_after->fe_idx;
        rc.mem_idx  = resume_after->mem_idx;
        bstf_advance(L->trace, &rc, 1);
        cur.sv_rec_byte = rc.rec_byte;
        cur.sv_fe_idx   = rc.fe_idx;
        cur.sv_mem_idx  = rc.mem_idx;
    }
    BSTM_HARD_ASSERT(cur.sv_rec_byte > L->hi_resume_byte || L->n_enter == 0,
        "還原點倒退：新 resume=%llu <= 歷史最大 %llu",
        (unsigned long long)cur.sv_rec_byte, (unsigned long long)L->hi_resume_byte);

    L->cur            = cur;
    L->last_br        = cur;
    L->has_last_br    = 1;
    L->branch_byte    = br->rec_byte;
    L->hi_branch_byte = br->rec_byte;
    L->hi_resume_byte = cur.sv_rec_byte;
    L->fstate         = BSTM_FS_SHADOW;
    L->n_enter++;
    r->n_enter_shadow++;
    return 1;
}

static void lane_enter_shadow(bstm_refill_t *r, bstm_lane_t *L)
{
    int i;

    if (L->fstate != BSTM_FS_CORRECT) {     /* 已經在推測了，忽略 */
        r->n_redir_in_shadow++;
        return;
    }
    /* (0) 過期請求：window 第 0 筆還在「剛剛才解析完」的那個 block 裡。
     *     分支只會解析一次，第二次的 redirect 要求是舊的，忽略。
     *     這個情況一定會發生：還原點是分支 + 1，而分支本身可能在 flush 之後
     *     才被交付出去（它比分支早 fetch），此時模型會再看到同一個 block 的
     *     FE_REDIRECT。不擋掉就會試圖重進同一個 shadow -> 游標倒退。 */
    if (L->cnt && L->resolved_valid &&
        L->pos[L->head].fe_idx == L->resolved_fe_idx) {
        L->n_resolved++;
        r->n_redir_resolved++;
        return;
    }
    /* (A) 分支還在 staging 裡（尚未交付） */
    for (i = 0; i < (int)L->cnt; i++) {
        if (!BSTM_ENT_HASSH(L->stage[L->head + i])) continue;
        if (do_enter(r, L, &L->pos[L->head + i], NULL)) {
            L->cnt = (uint16_t)(i + 1);      /* 保留到分支為止 */
            /* 這條分支已經用掉了：清掉 HASSH，避免它在 flush 之後被再挑一次 */
            L->stage[L->head + i] &= ~((uint64_t)1 << 48);
            return;
        }
        break;
    }
    /* (B) 分支在這一拍就被 take 掉了 —— fb_take 與 fb_redirect 同拍輸出，
     *     模型看到該 block 的 fe_event 時通常也一起消耗了那條分支。
     *     此時 staging 裡已經沒有它，要用消耗紀錄回推。
     *     還原點取「最後交付的那一筆 + 1」，不可倒退回分支 + 1。 */
    if (L->br_consumed_v) {
        if (do_enter(r, L, &L->br_consumed,
                     L->last_deliv_v ? &L->last_deliv : NULL)) {
            L->cnt = 0;                      /* 分支之後的推測 fetch 全丟 */
            return;
        }
    }
    L->n_nobranch++;
    r->n_redir_nobranch++;                   /* 不動游標 */
}


static void lane_leave_shadow(bstm_refill_t *r, bstm_lane_t *L)
{
    int i;
    if (L->fstate == BSTM_FS_CORRECT) return;         /* 沒在推測，no-op */

    /* staging 尾巴的 wrong-path entry 砍掉；correct-path 前綴留著 */
    for (i = 0; i < (int)L->cnt; i++) {
        if (BSTM_ENT_DUOP(L->stage[L->head + i]) >> 31) { L->cnt = (uint16_t)i; break; }
    }
    L->resolved_fe_idx = L->last_br.fe_idx;           /* 這個 block 解析完了 */
    L->resolved_valid  = 1;
    bstf_leave_shadow(L->trace, &L->cur);             /* cur = 分支 + 1 */
    BSTM_HARD_ASSERT(!L->cur.in_shadow, "leave_shadow 之後游標還在 shadow 裡");
    L->fstate = BSTM_FS_CORRECT;
    L->n_leave++;
    r->n_leave_shadow++;
    L->eof = 0;
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

        L->br_consumed_v = 0; L->last_deliv_v = 0;
        for (i = 0; i < (int)n; i++) {
            uint64_t ent = L->stage[L->head + i];
            if (BSTM_ENT_DUOP(ent) >> 31) { L->wp_consumed++; continue; }
            L->cp_consumed++;
            L->last_deliv   = L->pos[L->head + i];   /* 最後交付的 correct-path */
            L->last_deliv_v = 1;
            if (BSTM_ENT_HASSH(ent)) {               /* 同拍被消耗掉的分支 */
                L->br_consumed   = L->pos[L->head + i];
                L->br_consumed_v = 1;
            }
        }
        L->wrongpath = L->wp_consumed;
        L->head     += (uint16_t)n;
        L->cnt      -= (uint16_t)n;
        L->consumed += n;

        if (L->fstate == BSTM_FS_STARVED && L->cnt == 0) r->n_starve_cycles++;

        if ((redirect >> l) & 1) {
            r->n_redirect++;
            if ((redir_shadow >> l) & 1) lane_enter_shadow(r, L);
            else                         lane_leave_shadow(r, L);
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
        /* STARVED 不算跑完 —— 它在等 flush，模型還會前進。 */
        if (!L->active || (L->eof && L->cnt == 0 && L->fstate == BSTM_FS_CORRECT))
            m |= (uint64_t)1 << l;
    }
    return m;
}

uint64_t bstm_refill_starved_mask(const bstm_refill_t *r)
{
    uint64_t m = 0;
    int l;
    for (l = 0; l < BSTM_LANES; l++) {
        const bstm_lane_t *L = &r->lane[l];
        if (L->active && L->fstate == BSTM_FS_STARVED && L->cnt == 0)
            m |= (uint64_t)1 << l;
    }
    return m;
}

int bstm_refill_check(const bstm_refill_t *r, int lane, int at_eof,
                      char *msg, size_t msglen)
{
    const bstm_lane_t *L = &r->lane[lane];
    uint64_t in_flight = (L->fstate == BSTM_FS_CORRECT) ? 0 : 1;

    if (!L->active) { if (msg && msglen) msg[0] = 0; return 0; }

    if (L->n_enter != L->n_leave + in_flight) {
        snprintf(msg, msglen, "進出 shadow 不配對：enter=%llu leave=%llu in_flight=%llu",
                 (unsigned long long)L->n_enter, (unsigned long long)L->n_leave,
                 (unsigned long long)in_flight);
        return 1;
    }
    if (L->cp_consumed + L->wp_consumed != L->consumed) {
        snprintf(msg, msglen, "cp+wp != consumed：%llu + %llu != %llu",
                 (unsigned long long)L->cp_consumed, (unsigned long long)L->wp_consumed,
                 (unsigned long long)L->consumed);
        return 1;
    }
    if (at_eof && L->cp_consumed != L->trace->n_records) {
        snprintf(msg, msglen,
                 "correct-path 記錄沒有恰好消耗一次：cp_consumed=%llu n_records=%llu (差 %lld)",
                 (unsigned long long)L->cp_consumed,
                 (unsigned long long)L->trace->n_records,
                 (long long)L->cp_consumed - (long long)L->trace->n_records);
        return 1;
    }
    if (msg && msglen) msg[0] = 0;
    return 0;
}
