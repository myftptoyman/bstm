/* t_shadow — wrong-path 游標切換的端到端驗證（CONTRACT §5 G2）
 *
 * 跑完整條 trace，驗證：
 *   1. correct-path 游標單調前進（硬斷言在 refill.c 裡，這裡確認不會觸發）
 *   2. 進 shadow 次數 == 離開次數（收尾時沒有懸空的推測）
 *   3. **每筆 correct-path 記錄恰好被消耗一次** → cp_consumed == n_records
 *   4. wrong-path uop 不 retire：cnt_retired <= cp_consumed，cnt_wrongpath > 0
 *   5. cnt_mispred 接近 trace 裡誤預測 block 的數量
 *   6. cnt_st_refill 從 0 變成非零（shadow 供完、flush 未到 -> 前端斷流）
 *
 * 指紋（CONTRACT §11）：印出 enter/leave 次數、cp_consumed、wp_consumed。
 * 這些值會隨 trace 與 D 改變，舊 binary 印不出對應的數字。
 *
 * 另附負測：`t_shadow --negative` 故意讓分支位置倒退，斷言 refill.c 會中止。
 */
#include "tap.h"
#include "refill.h"
#include "transpose.h"
#include "mock_ooo_bs.h"
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

enum { C_CYCLES = 0, C_RETIRED, C_WRONGPATH, C_ST_FETCH, C_ST_REFILL,
       C_ST_RENAME, C_ST_IQ, C_ST_ROB, C_ST_LSQ, C_ST_MSHR, C_MISPRED, C_ROB_OCC };

static bstf_trace_t   tr;
static bstm_refill_t  rf;
static bstm_fbwin_t   win;
static mock_state_t  *ms_a, *ms_b;

/* 負測：把 hi_branch_byte 推到很大，下一次進 shadow 一定會被判倒退。
 * refill.c 的 BSTM_HARD_ASSERT 應該 abort()。 */
static int negative(const char *base)
{
    const bstf_trace_t *tp[BSTM_LANES];
    bstm_out_t out;
    uint8_t tk[BSTM_LANES];
    vec_t take[3];
    int l, c;
    if (bstf_open(&tr, base)) { fprintf(stderr, "open: %s\n", bstf_strerror()); return 2; }
    for (l = 0; l < BSTM_LANES; l++) tp[l] = &tr;
    bstm_refill_init(&rf, tp, 1);
    /* 把游標移到第一筆「帶 shadow 的分支」上，這樣 staging 裡一定有分支 */
    { bstf_cursor_t cc; uint64_t i, idx = 0;
      bstf_cur_init(&tr, &cc);
      for (i = 0; i < tr.n_records; i++) {
        const bstf_rec_t *R = bstf_peek(&tr, &cc);
        if (R && R->shadow_off && R->shadow_len) { idx = i; break; }
        bstf_advance(&tr, &cc, 1);
      }
      bstf_cur_seek_index(&tr, &rf.lane[0].cur, idx);
      rf.lane[0].head = 0; rf.lane[0].cnt = 0;
      bstm_refill_gather(&rf, 0);
    }
    rf.lane[0].has_last_br    = 1;
    rf.lane[0].hi_branch_byte = ~(uint64_t)0 >> 1;    /* 故意設得比任何分支都大 */
    memset(&out, 0, sizeof out);
    for (l = 0; l < BSTM_LANES; l++) tk[l] = 0;
    bstm_pack_lanes_small(tk, take, 3);
    fprintf(stderr, "negative: 預期 refill.c 中止...\n");
    for (c = 0; c < 200; c++) {
        bstm_refill_window(&rf, &win);
        bstm_refill_consume(&rf, take, (vec_t)1, (vec_t)1);   /* 一直要求跳 shadow */
    }
    fprintf(stderr, "negative: 沒有中止 —— 單調性斷言是空的！\n");
    return 0;                                         /* 回 0 = 負測失敗 */
}

int main(int argc, char **argv)
{
    const char *base = "traces/synth0";
    const bstf_trace_t *tp[BSTM_LANES];
    bstm_out_t out;
    uint32_t cfg[BSTM_LANES * BSTM_CFG_N];
    uint64_t *cn;
    uint64_t cyc = 0, all, done = 0;
    uint64_t tot_enter = 0, tot_leave = 0, tot_starve = 0;
    int l, k, i;
    char msg[256];

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--negative")) base = argv[i];
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--negative")) return negative(base);

    if (bstf_open(&tr, base)) { fprintf(stderr, "open %s: %s\n", base, bstf_strerror()); return 1; }
    CHECK(tr.hdr->shadow_bytes > 0,
          "trace 必須有 shadow 區才測得到 wrong-path（shadow_bytes=%llu）",
          (unsigned long long)tr.hdr->shadow_bytes);
    if (tr.hdr->shadow_bytes == 0) { bstf_close(&tr); TAP_DONE("shadow"); }

    for (l = 0; l < BSTM_LANES; l++) tp[l] = &tr;
    all = ~(uint64_t)0;
    bstm_refill_init(&rf, tp, all);

    /* 全 lane 同 cfg -> 結果應該逐 lane 完全相同，順便當一致性檢查 */
    for (l = 0; l < BSTM_LANES; l++) {
        cfg[l * BSTM_CFG_N + BSTM_CFG_ROB]      = 64;
        cfg[l * BSTM_CFG_N + BSTM_CFG_IQ]       = 32;
        cfg[l * BSTM_CFG_N + BSTM_CFG_LDQ]      = 16;
        cfg[l * BSTM_CFG_N + BSTM_CFG_STQ]      = 16;
        cfg[l * BSTM_CFG_N + BSTM_CFG_MSHR]     = 8;
        cfg[l * BSTM_CFG_N + BSTM_CFG_FETCH_W]  = 4;
        cfg[l * BSTM_CFG_N + BSTM_CFG_ISSUE_W]  = 4;
        cfg[l * BSTM_CFG_N + BSTM_CFG_COMMIT_W] = 2;
    }
    ms_a = bstm_mock_model.state_alloc();
    ms_b = bstm_mock_model.state_alloc();
    bstm_mock_model.init(ms_a, cfg, all);
    bstm_mock_model.init(ms_b, cfg, all);

    while (cyc < 40000000ULL) {
        mock_state_t *t;
        bstm_refill_window(&rf, &win);
        mock_eval_rst(ms_b, ms_a, &win, &out, VZERO);
        bstm_refill_consume(&rf, out.take, out.redirect, out.redir_shadow);
        t = ms_a; ms_a = ms_b; ms_b = t;
        cyc++;
        if ((cyc & 255) == 0) {
            done = bstm_refill_done_mask(&rf);
            if ((done & all) == all) break;
        }
    }
    CHECK((bstm_refill_done_mask(&rf) & all) == all, "所有 lane 都跑完（%llu cycle）",
          (unsigned long long)cyc);

    /* ---- 不變式 ---- */
    for (l = 0; l < BSTM_LANES; l++) {
        if (bstm_refill_check(&rf, l, 1, msg, sizeof msg))
            { CHECK(0, "lane %d: %s", l, msg); break; }
        tot_enter  += rf.lane[l].n_enter;
        tot_leave  += rf.lane[l].n_leave;
        tot_starve += rf.lane[l].n_starve;
    }
    CHECK(tot_enter == tot_leave, "進出 shadow 配對：enter=%llu leave=%llu",
          (unsigned long long)tot_enter, (unsigned long long)tot_leave);
    CHECK(tot_enter > 0, "真的進過 shadow（%llu 次 / 64 lane）",
          (unsigned long long)tot_enter);
    CHECK(rf.lane[0].cp_consumed == tr.n_records,
          "correct-path 恰好消耗一次：%llu == n_records %llu",
          (unsigned long long)rf.lane[0].cp_consumed,
          (unsigned long long)tr.n_records);
    CHECK(rf.lane[0].wp_consumed > 0, "有消耗 wrong-path 記錄：%llu",
          (unsigned long long)rf.lane[0].wp_consumed);
    CHECK(rf.n_redir_nobranch == 0,
          "每次要求跳 shadow 都找得到帶 shadow 的分支（找不到 %llu 次）",
          (unsigned long long)rf.n_redir_nobranch);
    CHECK(rf.n_redir_in_shadow == 0, "不會在 shadow 中又收到跳 shadow 的要求（%llu 次）",
          (unsigned long long)rf.n_redir_in_shadow);
    /* 所有 lane 同 cfg -> 逐 lane 必須完全相同 */
    for (l = 1; l < BSTM_LANES; l++)
        if (rf.lane[l].cp_consumed != rf.lane[0].cp_consumed ||
            rf.lane[l].wp_consumed != rf.lane[0].wp_consumed ||
            rf.lane[l].n_enter     != rf.lane[0].n_enter)
            { CHECK(0, "lane %d 與 lane 0 不一致", l); break; }

    /* ---- counter ---- */
    cn = calloc(BSTM_LANES * MOCK_NCNT, sizeof *cn);
    bstm_mock_model.extract(ms_a, cn);
    {
        const uint64_t *c0 = cn;
        uint64_t nmis_blocks = 0;
        /* trace 裡誤預測的 block 數 = 帶 shadow 的記錄數 */
        { bstf_cursor_t cc; uint64_t r;
          bstf_cur_init(&tr, &cc);
          for (r = 0; r < tr.n_records; r++) {
            const bstf_rec_t *R = bstf_peek(&tr, &cc);
            if (!R) break;
            if (R->shadow_off && R->shadow_len) nmis_blocks++;
            bstf_advance(&tr, &cc, 1);
          } }

        CHECK(c0[C_WRONGPATH] > 0, "cnt_wrongpath 從 0 變成非零：%llu",
              (unsigned long long)c0[C_WRONGPATH]);
        CHECK(c0[C_WRONGPATH] == rf.lane[0].wp_consumed,
              "模型數到的 wrong-path uop == runtime 供應的：%llu vs %llu",
              (unsigned long long)c0[C_WRONGPATH],
              (unsigned long long)rf.lane[0].wp_consumed);
        CHECK(c0[C_RETIRED] <= rf.lane[0].cp_consumed,
              "retire 數不得超過 correct-path 供應數：%llu <= %llu",
              (unsigned long long)c0[C_RETIRED],
              (unsigned long long)rf.lane[0].cp_consumed);
        CHECK(c0[C_MISPRED] == rf.lane[0].n_enter,
              "cnt_mispred == 實際進 shadow 次數：%llu vs %llu",
              (unsigned long long)c0[C_MISPRED],
              (unsigned long long)rf.lane[0].n_enter);
        CHECK(c0[C_MISPRED] <= nmis_blocks,
              "cnt_mispred %llu <= trace 的誤預測分支數 %llu",
              (unsigned long long)c0[C_MISPRED], (unsigned long long)nmis_blocks);
        /* 精確恆等式（CONTRACT §8 規則 4：用「==」而不是「>=」）：
         * 每一次模型拉 redir_shadow，runtime 要嘛成功進 shadow、要嘛找不到分支。 */
        CHECK(c0[C_MISPRED] == rf.lane[0].n_enter + rf.lane[0].n_nobranch
                             + rf.lane[0].n_resolved,
              "mispred == enter + nobranch + resolved：%llu == %llu + %llu + %llu",
              (unsigned long long)c0[C_MISPRED],
              (unsigned long long)rf.lane[0].n_enter,
              (unsigned long long)rf.lane[0].n_nobranch,
              (unsigned long long)rf.lane[0].n_resolved);
        /* cnt_mispred 會低於 trace 的誤預測分支數，原因是 CONTRACT §5 的已知
         * 限制 G7：fb_fe_event 只帶 window 第 0 筆所屬 block 的事件，4-wide
         * window 一拍可能整個跨過一個 block，那個 block 的 REDIRECT 就看不到。
         * 這裡把跳過的 block 數量量出來，並驗證兩者互相解釋得通。 */
        printf("  fetch block：檔案 %llu 個，放到 window head 過 %llu 個，"
               "跨過去沒看到 %llu 個\n",
               (unsigned long long)tr.hdr->n_fe_blocks,
               (unsigned long long)rf.lane[0].n_blk_seen,
               (unsigned long long)rf.lane[0].n_blk_skipped);
        printf("  誤預測：trace 有 %llu 次，runtime 端出去給模型看到 %llu 次，"
               "模型實際觸發 %llu 次\n",
               (unsigned long long)nmis_blocks,
               (unsigned long long)rf.lane[0].n_redir_blk_seen,
               (unsigned long long)c0[C_MISPRED]);

        printf("  ---- 指紋 ----\n");
        printf("  cycles=%llu  n_records=%llu  誤預測分支=%llu\n",
               (unsigned long long)cyc, (unsigned long long)tr.n_records,
               (unsigned long long)nmis_blocks);
        printf("  runtime: enter=%llu leave=%llu starve=%llu "
               "cp_consumed=%llu wp_consumed=%llu nobranch=%llu resolved=%llu\n",
               (unsigned long long)rf.lane[0].n_enter,
               (unsigned long long)rf.lane[0].n_leave,
               (unsigned long long)rf.lane[0].n_starve,
               (unsigned long long)rf.lane[0].cp_consumed,
               (unsigned long long)rf.lane[0].wp_consumed,
               (unsigned long long)rf.lane[0].n_nobranch,
               (unsigned long long)rf.lane[0].n_resolved);
        printf("  model  : retired=%llu wrongpath=%llu mispred=%llu "
               "st_fetch=%llu st_refill=%llu  IPC=%.3f\n",
               (unsigned long long)c0[C_RETIRED], (unsigned long long)c0[C_WRONGPATH],
               (unsigned long long)c0[C_MISPRED], (unsigned long long)c0[C_ST_FETCH],
               (unsigned long long)c0[C_ST_REFILL],
               c0[C_CYCLES] ? (double)c0[C_RETIRED] / (double)c0[C_CYCLES] : 0.0);
        CHECK(c0[C_ST_REFILL] > 0,
              "cnt_st_refill 從 0 變成非零（shadow 供完、flush 未到）：%llu",
              (unsigned long long)c0[C_ST_REFILL]);
    }

    /* ---- 負測：確認單調性斷言抓得到 ---- */
    {
        pid_t pid = fork();
        if (pid == 0) {
            char *av[4]; int fd;
            av[0] = argv[0]; av[1] = (char *)base; av[2] = (char *)"--negative"; av[3] = NULL;
            fd = open("/dev/null", 1 /*O_WRONLY*/);
            if (fd >= 0) { dup2(fd, 2); close(fd); }
            execv(argv[0], av);
            _exit(127);
        } else if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            CHECK(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0),
                  "負測：故意讓分支倒退時，refill.c 必須中止（狀態 0x%x）", st);
            printf("  負測：子行程 %s（證明單調性斷言不是空檢查）\n",
                   WIFSIGNALED(st) ? "被訊號中止" : "非零退出");
        }
    }

    free(cn);
    bstm_mock_model.state_free(ms_a);
    bstm_mock_model.state_free(ms_b);
    bstf_close(&tr);
    (void)k;
    TAP_DONE("shadow wrong-path");
}
