/* t_refill_wp — 「shadow 供完但 flush 還沒到」的行為（協調者任務第 3 點）
 *
 * 決定的行為：**停在 shadow 裡、停止供應 uop**（fb_valid 全 0），
 * 游標不動，等 fb_redir_shadow=0 才還原。
 * 不可以自己溜回正確路徑 —— 那會讓模型在還以為自己在推測時
 * 收到正確路徑的 uop 並且把它們 retire 掉。
 *
 * 本測試單獨驅動 lane 0，不透過模型，所以可以精確控制時序：
 *   1. 走到第一筆帶 shadow 的分支，拉 redirect+shadow
 *   2. 一路吃到 shadow 用完 -> 斷言 starved、fb_valid == 0、游標還在 shadow
 *   3. 多等 50 拍 -> 斷言還是 starved（不會自己跑掉）
 *   4. 拉 redirect+~shadow -> 斷言回到分支的下一筆、之後每筆都是 correct path
 */
#include "tap.h"
#include "refill.h"
#include "transpose.h"
#include <string.h>

static bstf_trace_t  tr;
static bstm_refill_t rf;
static bstm_fbwin_t  win;

static void step(unsigned take_n, int redir, int shadow)
{
    uint8_t tk[BSTM_LANES];
    vec_t t3[3];
    int l;
    for (l = 0; l < BSTM_LANES; l++) tk[l] = 0;
    tk[0] = (uint8_t)take_n;
    bstm_pack_lanes_small(tk, t3, 3);
    bstm_refill_window(&rf, &win);
    bstm_refill_consume(&rf, t3, redir ? (vec_t)1 : VZERO, shadow ? (vec_t)1 : VZERO);
}

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "traces/synth0";
    const bstf_trace_t *tp[BSTM_LANES];
    bstm_lane_t *L;
    uint64_t br_idx = 0, br_byte = 0, resume_byte;
    uint16_t slen = 0;
    int i, c, wp_seen = 0;

    if (bstf_open(&tr, base)) { fprintf(stderr, "open: %s\n", bstf_strerror()); return 1; }
    for (i = 0; i < BSTM_LANES; i++) tp[i] = &tr;

    /* 找第一筆帶 shadow 的分支 */
    { bstf_cursor_t cc; uint64_t k;
      bstf_cur_init(&tr, &cc);
      for (k = 0; k < tr.n_records; k++) {
        const bstf_rec_t *R = bstf_peek(&tr, &cc);
        if (R && R->shadow_off && R->shadow_len) {
            br_idx = k; br_byte = cc.rec_byte; slen = R->shadow_len; break;
        }
        bstf_advance(&tr, &cc, 1);
      } }
    CHECK(slen > 0, "找到帶 shadow 的分支（index %llu, shadow_len %u）",
          (unsigned long long)br_idx, slen);
    if (!slen) { bstf_close(&tr); TAP_DONE("refill starve"); }
    resume_byte = br_byte + tr.rec_bytes;

    bstm_refill_init(&rf, tp, 1);
    L = &rf.lane[0];
    /* 把游標移到分支上，讓它成為 staging 的第一筆 */
    bstf_cur_seek_index(&tr, &L->cur, br_idx);
    L->head = 0; L->cnt = 0;
    bstm_refill_gather(&rf, 0);

    /* 1. 拉 redirect + shadow */
    step(0, 1, 1);
    CHECK(L->fstate == BSTM_FS_SHADOW, "進了 shadow（fstate=%d）", L->fstate);
    CHECK(L->n_enter == 1, "enter=1");
    CHECK(L->cur.sv_rec_byte == resume_byte,
          "還原點 == 分支的下一筆：%llu == %llu",
          (unsigned long long)L->cur.sv_rec_byte, (unsigned long long)resume_byte);

    /* 2. 一路吃到 shadow 用完 */
    for (c = 0; c < (int)slen + 40; c++) {
        uint32_t d0;
        int b;
        bstm_refill_window(&rf, &win);
        if ((win.valid[0] >> 0) & 1) {
            d0 = 0;
            for (b = 0; b < 32; b++) d0 |= (uint32_t)((win.duop[b] >> 0) & 1) << b;
            if (d0 >> 31) wp_seen++;
        }
        { uint8_t tk[BSTM_LANES]; vec_t t3[3]; int l;
          for (l = 0; l < BSTM_LANES; l++) tk[l] = 0;
          tk[0] = 1;
          bstm_pack_lanes_small(tk, t3, 3);
          bstm_refill_consume(&rf, t3, VZERO, VZERO); }
        if (L->fstate == BSTM_FS_STARVED && L->cnt == 0) break;
    }
    CHECK(L->fstate == BSTM_FS_STARVED, "shadow 供完之後進入 STARVED（fstate=%d）", L->fstate);
    CHECK(wp_seen >= slen - BSTM_W,
          "wrong-path uop 有被交付出去：%d 筆（shadow_len=%u）", wp_seen, slen);
    CHECK((bstm_refill_starved_mask(&rf) & 1) != 0, "starved_mask 有 lane 0");
    CHECK((bstm_refill_done_mask(&rf) & 1) == 0, "STARVED 不算跑完");

    /* 3. 再等 50 拍：不可以自己溜回正確路徑 */
    for (c = 0; c < 50; c++) {
        bstm_refill_window(&rf, &win);
        CHECK(((win.valid[0] | win.valid[1] | win.valid[2] | win.valid[3]) & 1) == 0,
              "STARVED 期間 fb_valid 必須是 0（第 %d 拍）", c);
        if (((win.valid[0] >> 0) & 1)) break;
        step(0, 0, 0);
    }
    CHECK(L->fstate == BSTM_FS_STARVED, "50 拍之後還是 STARVED（不會自己回正確路徑）");
    CHECK(L->cur.in_shadow, "游標還在 shadow 裡（還原點沒丟）");
    /* 只有分支本身（它是 correct-path 指令、比 shadow 早 fetch）會被交付，
     * 之後一筆 correct-path 都不該漏出去。 */
    CHECK(L->cp_consumed == 1,
          "shadow 期間只交付了分支本身這 1 筆 correct-path（實際 %llu）",
          (unsigned long long)L->cp_consumed);

    /* 4. flush 到達 */
    step(0, 1, 0);
    CHECK(L->fstate == BSTM_FS_CORRECT, "回到正確路徑（fstate=%d）", L->fstate);
    CHECK(L->n_leave == 1, "leave=1");
    CHECK(!L->cur.in_shadow, "游標離開 shadow");
    bstm_refill_window(&rf, &win);
    CHECK(((win.valid[0] >> 0) & 1) != 0, "flush 之後又有 uop 可供");
    CHECK(L->pos[L->head].rec_byte == resume_byte,
          "第一筆是分支的下一筆：%llu == %llu",
          (unsigned long long)L->pos[L->head].rec_byte, (unsigned long long)resume_byte);
    { uint32_t d0 = 0; int b;
      for (b = 0; b < 32; b++) d0 |= (uint32_t)((win.duop[b] >> 0) & 1) << b;
      CHECK(((d0 >> 31) & 1) == 0, "而且是 correct path（DUOP_WRONGPATH=0）"); }

    printf("  starve: shadow_len=%u 交付 %d 筆 wrong-path，enter=%llu leave=%llu "
           "starve=%llu，還原點 byte=%llu\n",
           slen, wp_seen, (unsigned long long)L->n_enter,
           (unsigned long long)L->n_leave, (unsigned long long)L->n_starve,
           (unsigned long long)resume_byte);
    bstf_close(&tr);
    TAP_DONE("refill starve");
}
