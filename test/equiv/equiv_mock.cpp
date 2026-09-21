/* ============================================================
 * equiv_mock — test/mock/mock_ooo_top.v 的對拍
 *   Verilator（1 instance）  vs  手寫 bit-sliced mock（lane 0）
 * 比對 fb_take / fb_redirect / fb_redir_shadow 與全部 11 個 48-bit counter。
 *
 * 這一支同時驗證兩件事：
 *   1. 對拍框架能處理「有 counter 的完整 top-level」
 *   2. runtime 的 refill 路徑（真 .bstf -> bit-sliced window）接得起來
 * ============================================================ */
#include "Vmock_ooo_top.h"
#include "verilated.h"
#include "equiv.h"
#include <cstdlib>
#include <cstring>

extern "C" {
#include "bstm_types.h"
#include "bstf_reader.h"
#include "refill.h"
#include "bstm_abi.h"
#include "mock_ooo_bs.h"
#include "transpose.h"
}

struct Scalar {                      /* lane 0 的純量輸入 */
    uint8_t  valid;                  /* [3:0]   */
    uint32_t duop[4];
    uint8_t  fe;
    uint32_t mem;                    /* 4 x 8 bit */
};

struct Dut {
    Vmock_ooo_top *v;
    mock_state_t  *cur, *nxt;
    EquivCtx       ctx;
    long           n_take = 0, n_redir = 0, n_shadow = 0;
    uint8_t        take_seen = 0;

    Dut() {
        v = new Vmock_ooo_top;
        cur = (mock_state_t *)calloc(1, sizeof(mock_state_t));
        nxt = (mock_state_t *)calloc(1, sizeof(mock_state_t));
        cur->enable = VONES; nxt->enable = VONES;
    }
    ~Dut() { delete v; free(cur); free(nxt); }

    /* 只改 cfg，不動暫存器/counter —— 換 config 之後要自己 assert rst。
     * 其他 lane 灌不同的合法 cfg，順便驗證沒有跨 lane 污染。 */
    void set_cfg(const uint32_t cfg0[BSTM_CFG_N], uint64_t noise)
    {
        for (int f = 0; f < BSTM_CFG_N; f++) {
            uint8_t val[BSTM_LANES];
            for (int l = 0; l < BSTM_LANES; l++)
                val[l] = (uint8_t)(l ? (1 + ((noise >> (f * 3 + l)) & 3)) : cfg0[f]);
            bstm_pack_lanes_small(val, cur->cfg[f], 8);
            memcpy(nxt->cfg[f], cur->cfg[f], sizeof cur->cfg[f]);
        }
        v->cfg_rob_entries  = (uint8_t)cfg0[BSTM_CFG_ROB];
        v->cfg_iq_entries   = (uint8_t)cfg0[BSTM_CFG_IQ];
        v->cfg_ldq_entries  = (uint8_t)cfg0[BSTM_CFG_LDQ];
        v->cfg_stq_entries  = (uint8_t)cfg0[BSTM_CFG_STQ];
        v->cfg_mshr_entries = (uint8_t)cfg0[BSTM_CFG_MSHR];
        v->cfg_fetch_width  = (uint8_t)cfg0[BSTM_CFG_FETCH_W];
        v->cfg_issue_width  = (uint8_t)cfg0[BSTM_CFG_ISSUE_W];
        v->cfg_commit_width = (uint8_t)cfg0[BSTM_CFG_COMMIT_W];
    }

    /* win 已經是 bit-sliced；s 是 lane 0 抽出來的純量。 */
    bool step(const bstm_fbwin_t *win, const Scalar &s, uint8_t rst, bstm_out_t *out)
    {
        v->rst = rst;
        v->fb_valid = s.valid;
        for (int i = 0; i < 4; i++) v->fb_duop[i] = s.duop[i];
        v->fb_fe_event  = s.fe;
        v->fb_mem_event = s.mem;
        v->clk = 0; v->eval();

        mock_eval_rst(nxt, cur, win, out, rst ? VONES : VZERO);

        bool ok = true;
        uint64_t tk = bs_get(out->take, 3, 0);
        ok &= ctx.cmp("fb_take",         v->fb_take,         tk, 3);
        ok &= ctx.cmp("fb_redirect",     v->fb_redirect,     (out->redirect     >> 0) & 1, 1);
        ok &= ctx.cmp("fb_redir_shadow", v->fb_redir_shadow, (out->redir_shadow >> 0) & 1, 1);

        v->clk = 1; v->eval();
        mock_state_t *t = cur; cur = nxt; nxt = t;

        /* counter 是同步暫存器，clk 上升後才比 */
        static const char *cn[MOCK_NCNT] = {
            "cnt_cycles","cnt_retired","cnt_wrongpath","cnt_st_fetch","cnt_st_rename",
            "cnt_st_iq","cnt_st_rob","cnt_st_lsq","cnt_st_mshr","cnt_mispred","cnt_rob_occ_sum" };
        uint64_t vref[MOCK_NCNT] = {
            v->cnt_cycles, v->cnt_retired, v->cnt_wrongpath, v->cnt_st_fetch,
            v->cnt_st_rename, v->cnt_st_iq, v->cnt_st_rob, v->cnt_st_lsq,
            v->cnt_st_mshr, v->cnt_mispred, v->cnt_rob_occ_sum };
        for (int i = 0; i < MOCK_NCNT; i++)
            ok &= ctx.cmp(cn[i], vref[i], bs_get(cur->cnt[i], BSTM_CNT_W, 0), BSTM_CNT_W);

        take_seen |= (uint8_t)(1u << tk);
        n_take  += (long)tk;
        n_redir += (out->redirect     >> 0) & 1;
        n_shadow+= (out->redir_shadow >> 0) & 1;
        ctx.cycle++;
        return ok;
    }
};

/* 把 bit-sliced window 的 lane 0 抽成純量 */
static Scalar extract0(const bstm_fbwin_t *w)
{
    Scalar s{};
    s.valid = (uint8_t)bs_get(w->valid, BSTM_W, 0);
    for (int e = 0; e < BSTM_W; e++) s.duop[e] = (uint32_t)bs_get(&w->duop[e * 32], 32, 0);
    s.fe = (uint8_t)bs_get(w->fe, 8, 0);
    s.mem = (uint32_t)bs_get(w->mem, 32, 0);
    return s;
}
/* 把純量寫進 lane 0，其餘 lane 灌雜訊 */
static void inject(bstm_fbwin_t *w, const Scalar &s, uint64_t noise)
{
    for (int b = 0; b < BSTM_W; b++) w->valid[b] = noise * (b + 1);
    for (int b = 0; b < BSTM_W * 32; b++) w->duop[b] = noise * (uint64_t)(b + 7);
    for (int b = 0; b < 8; b++) w->fe[b] = noise * (uint64_t)(b + 11);
    for (int b = 0; b < BSTM_W * 8; b++) w->mem[b] = noise * (uint64_t)(b + 3);
    bs_put(w->valid, BSTM_W, 0, s.valid);
    for (int e = 0; e < BSTM_W; e++) bs_put(&w->duop[e * 32], 32, 0, s.duop[e]);
    bs_put(w->fe, 8, 0, s.fe);
    bs_put(w->mem, 32, 0, s.mem);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    long N = 200000;
    const char *tbase = "traces/synth0";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] >= '0' && argv[i][0] <= '9') N = atol(argv[i]);
        else tbase = argv[i];
    }
    Dut d;
    Lfsr lf;
    static bstm_fbwin_t win;
    bstm_out_t out;
    bool stop = false;
    uint32_t cfg0[BSTM_CFG_N] = { 64, 32, 16, 16, 8, 4, 4, 2 };

    auto do_reset = [&](int n) {
        Scalar z{};
        memset(&win, 0, sizeof win);
        for (int i = 0; i < n && !stop; i++) stop = !d.step(&win, z, 1, &out);
    };

    /* ---- reset ---- */
    d.set_cfg(cfg0, 0);
    d.ctx.set_phase("reset");
    do_reset(4);

    /* ---- (a) LFSR 隨機 ---- */
    d.ctx.set_phase("lfsr-random");
    for (long i = 0; i < N && !stop; i++) {
        uint64_t x = lf.next();
        Scalar s{};
        s.valid = (uint8_t)(x & 0xF);
        for (int e = 0; e < 4; e++) s.duop[e] = (uint32_t)(x >> e) * 2654435761u;
        s.fe  = (uint8_t)((x >> 17) & 0xFF);
        s.mem = (uint32_t)(x >> 23);
        inject(&win, s, x);
        stop = !d.step(&win, s, 0, &out);
    }

    /* ---- (b) 真實 .bstf，走完整 refill 路徑 ---- */
    static bstf_trace_t tr;
    if (!bstf_open(&tr, tbase)) {
        static bstm_refill_t rf;
        const bstf_trace_t *tp[BSTM_LANES];
        for (int l = 0; l < BSTM_LANES; l++) tp[l] = &tr;
        bstm_refill_init(&rf, tp, VONES);
        d.ctx.set_phase("bstf-replay");
        uint32_t cfg1[BSTM_CFG_N] = { 32, 16, 16, 16, 8, 4, 4, 2 };
        d.set_cfg(cfg1, 0); do_reset(2);
        for (long i = 0; i < N && !stop; i++) {
            bstm_refill_window(&rf, &win);
            Scalar s = extract0(&win);
            stop = !d.step(&win, s, 0, &out);
            bstm_refill_consume(&rf, out.take, out.redirect, out.redir_shadow);
            if (bstm_refill_done_mask(&rf) & 1) break;
        }
        bstf_close(&tr);
    } else {
        std::fprintf(stderr, "note: %s 打不開，跳過 bstf-replay：%s\n", tbase, bstf_strerror());
    }

    /* ---- (c) 定向邊界 ---- */
    d.ctx.set_phase("directed");
    struct Case { const char *name; uint32_t cfg[BSTM_CFG_N]; uint8_t valid, fe; int n; };
    const Case cases[] = {
        /*                       rob iq ldq stq mshr fw iw cw            valid fe    n  */
        { "ROB=1（永遠滿）",      {  1,32,16,16,8,4,4,2 }, 0xF, 0x10, 400 },
        { "ROB=64 commit=4 滿吞吐",{ 64,32,16,16,8,4,4,4 }, 0xF, 0x10, 400 },
        { "fetch_width=1",        { 64,32,16,16,8,1,4,2 }, 0xF, 0x10, 400 },
        { "commit=1（塞住）",     { 64,32,16,16,8,4,4,1 }, 0xF, 0x10, 400 },
        { "valid=0（前端斷流）",  { 64,32,16,16,8,4,4,2 }, 0x0, 0x00, 200 },
        { "每拍誤預測（REDIRECT & ~DIR_OK）", { 64,32,16,16,8,4,4,2 }, 0xF, 0x80, 600 },
        { "REDIRECT 但預測正確",  { 64,32,16,16,8,4,4,2 }, 0xF, 0x90, 300 },
        { "ROB 邊界 occ==rob",    {  8,32,16,16,8,4,4,1 }, 0xF, 0x10, 400 },
    };
    for (const auto &c : cases) {
        if (stop) break;
        d.set_cfg(c.cfg, 0);
        do_reset(2);            /* 換 config 之後兩邊一起 reset */
        for (int i = 0; i < c.n && !stop; i++) {
            Scalar s{};
            s.valid = c.valid;
            s.fe = (uint8_t)(c.fe | (i & 3));
            for (int e = 0; e < 4; e++) s.duop[e] = (uint32_t)(i * 2654435761u + e);
            s.mem = (uint32_t)(i * 97);
            inject(&win, s, lf.next());
            stop = !d.step(&win, s, 0, &out);
        }
    }

    /* ---- 同時 flush + fill：rst 與滿流量同一拍 ---- */
    d.ctx.set_phase("directed-reset-under-traffic");
    d.set_cfg(cfg0, 0); do_reset(2);
    for (int rep = 0; rep < 30 && !stop; rep++) {
        Scalar s{}; s.valid = 0xF; s.fe = 0x90;
        for (int e = 0; e < 4; e++) s.duop[e] = 0xdeadbeefu + e;
        s.mem = 0x01020304u;
        for (int i = 0; i < 7 && !stop; i++) { inject(&win, s, lf.next()); stop = !d.step(&win, s, 0, &out); }
        inject(&win, s, lf.next());
        stop = !d.step(&win, s, 1, &out);              /* rst 與滿流量同拍 */
        for (int i = 0; i < 7 && !stop; i++) { inject(&win, s, lf.next()); stop = !d.step(&win, s, 0, &out); }
    }

    std::printf("活動量: sum(fb_take)=%ld, fb_take 取過的值 = %s%s%s%s%s, "
                "redirect %ld 次 (其中跳 shadow %ld)\n",
                d.n_take,
                (d.take_seen & 1) ? "0 " : "", (d.take_seen & 2) ? "1 " : "",
                (d.take_seen & 4) ? "2 " : "", (d.take_seen & 8) ? "3 " : "",
                (d.take_seen & 16) ? "4" : "",
                d.n_redir, d.n_shadow);
    return d.ctx.report("equiv mock_ooo_top.v");
}
