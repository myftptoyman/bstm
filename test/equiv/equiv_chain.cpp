/* ============================================================
 * equiv_chain — prototype/chain.v（830 cell）的對拍
 *   Verilator（1 instance）  vs  bit-sliced C（lane 0）
 * 這是框架的第一個目標：證明流程可以跑，且對一個非平凡的設計
 * （32 級 stage + credit counter + $pmux）能 bit-exact。
 *
 * 測試向量：
 *   (a) LFSR 隨機
 *   (b) 真實 .bstf 的前 N 筆（uop class/lat/dst 映射到 chain 的輸入）
 *   (c) 定向邊界：credit 用盡 / 全滿 / rst 脈衝 / valid 連續拉高
 * ============================================================ */
#include "Vchain.h"
#include "verilated.h"
#include "equiv.h"
#include "gen/chain_bs.h"
#include <cstdlib>
#include <vector>

extern "C" {
#include "bstf.h"
}
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------
 * 已知限制（2026-09-21 由本框架抓到，已回報）：
 *   prototype/gen.py:156 算了 rv = "VZERO" 但從來沒用 —— 所有 $sdff 的
 *   reset value 都被當成 0。stage.v 的 credit 卻是 reset 成 {4{1'b1}}=15。
 *   因此只要 assert rst，bit-sliced 與 RTL 就會分岔。
 *   預設用「零初始化、完全不 assert rst」對拍（Verilator --x-initial 0
 *   讓兩邊起點一致）；加 -r 會把 rst 階段打開，用來示範框架抓得到它。
 * ------------------------------------------------------------------ */
struct Dut {
    Vchain      *v;
    chain_state_t *cur, *nxt;
    EquivCtx     ctx;
    long         n_ready = 0, n_valid = 0;
    uint64_t     tagseen = 0;

    Dut() {
        v = new Vchain;
        cur = (chain_state_t *)calloc(1, chain_state_size());
        nxt = (chain_state_t *)calloc(1, chain_state_size());
    }
    ~Dut() { delete v; free(cur); free(nxt); }

    /* 一個 target cycle：驅動輸入、比對輸出、推進狀態 */
    bool step(uint8_t rst, uint8_t in_valid, uint8_t in_tag, uint8_t in_lat,
              uint8_t dn_credit_ret, uint64_t noise)
    {
        vec_t vrst = rst ? VONES : VZERO;      /* 所有 lane 同步 reset */
        vec_t vval, vcr, vtag[6], vlat[4];
        vec_t rdy, ov, otag[6];

        /* lane 0 = 受測值；其餘 lane 灌雜訊，順便驗證沒有跨 lane 污染 */
        vval = noise & ~1ULL;                  if (in_valid)      vval |= 1;
        vcr  = (noise >> 7) & ~1ULL;           if (dn_credit_ret) vcr  |= 1;
        for (int b = 0; b < 6; b++) {
            vtag[b] = (noise * (uint64_t)(b + 3)) & ~1ULL;
            if ((in_tag >> b) & 1) vtag[b] |= 1;
        }
        for (int b = 0; b < 4; b++) {
            vlat[b] = (noise >> (b + 3)) & ~1ULL;
            if ((in_lat >> b) & 1) vlat[b] |= 1;
        }

        /* Verilator：clk=0 求組合輸出 */
        v->rst = rst; v->in_valid = in_valid; v->in_tag = in_tag;
        v->in_lat = in_lat; v->dn_credit_ret = dn_credit_ret;
        v->clk = 0; v->eval();

        chain_eval_cycle(nxt, cur, vrst, vval, vtag, vlat, vcr, &rdy, &ov, otag);

        bool ok = true;
        ok &= ctx.cmp("in_ready",  v->in_ready,  (rdy >> 0) & 1, 1);
        ok &= ctx.cmp("out_valid", v->out_valid, (ov  >> 0) & 1, 1);
        ok &= ctx.cmp("out_tag",   v->out_tag,   bs_get(otag, 6, 0), 6);

        /* 兩邊一起前進一拍 */
        n_ready += v->in_ready; n_valid += v->out_valid;
        tagseen |= 1ULL << (v->out_tag & 63);

        v->clk = 1; v->eval();
        chain_state_t *t = cur; cur = nxt; nxt = t;
        ctx.cycle++;
        return ok;
    }
};

/* ---- (b) 真實 .bstf ---- */
struct Trace {
    const uint8_t *m = nullptr; size_t len = 0;
    const bstf_rec_t *r = nullptr; uint64_t n = 0;
    bool open(const char *p) {
        int fd = ::open(p, O_RDONLY); struct stat st;
        if (fd < 0) return false;
        if (fstat(fd, &st) || st.st_size <= (off_t)sizeof(bstf_hdr_t)) { close(fd); return false; }
        m = (const uint8_t *)mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) { m = nullptr; return false; }
        len = st.st_size;
        const bstf_hdr_t *h = (const bstf_hdr_t *)m;
        if (h->magic != BSTF_MAGIC) return false;
        r = (const bstf_rec_t *)(m + sizeof(bstf_hdr_t));
        n = h->n_records;
        return true;
    }
};

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    Dut d;
    Lfsr lf;
    long N = 200000;
    const char *tpath = "traces/synth0.bstf";
    bool with_reset = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) with_reset = true;
        else if (argv[i][0] >= '0' && argv[i][0] <= '9') N = atol(argv[i]);
        else tpath = argv[i];
    }
    bool stop = false;
    long act_ready = 0, act_valid = 0;

    if (with_reset) {
        d.ctx.set_phase("reset");
        for (int i = 0; i < 4 && !stop; i++) stop = !d.step(1, 0, 0, 0, 0, 0);
    } else {
        std::printf("note: 跳過 rst（gen.py 把 $sdff reset value 當成 0，"
                    "stage.v 的 credit 是 15）。用 -r 打開 rst 階段。\n");
    }

    /* ---- (a) LFSR 隨機 ---- */
    d.ctx.set_phase("lfsr-random");
    for (long i = 0; i < N && !stop; i++) {
        uint64_t x = lf.next();
        stop = !d.step(0, x & 1, (x >> 1) & 63, (x >> 7) & 15, (x >> 11) & 1, x);
    }

    /* ---- (b) 真實 .bstf 的前 N 筆 ---- */
    Trace tr;
    if (tr.open(tpath)) {
        d.ctx.set_phase("bstf-replay");
        uint64_t lim = tr.n; if ((long)lim > N) lim = N;
        for (uint64_t i = 0; i < lim && !stop; i++) {
            const bstf_rec_t *R = &tr.r[i];
            uint8_t tag = (uint8_t)(R->dst & 0x3F);
            uint8_t lat = (uint8_t)(R->exec_lat & 0xF);
            uint8_t vld = 1;
            uint8_t crd = (uint8_t)((R->uop_class == UC_LOAD) ? 1 : (i & 1));
            stop = !d.step(0, vld, tag, lat, crd, lf.next());
        }
    } else {
        std::fprintf(stderr, "note: %s 打不開，跳過 bstf-replay 階段\n", tpath);
    }

    /* ---- (c) 定向邊界 ---- */
    d.ctx.set_phase("directed");
    struct Pat { const char *name; uint8_t v, crd; int n; };
    const Pat pats[] = {
        { "valid 連續拉滿、不還 credit（credit 用盡）", 1, 0, 300 },
        { "valid 全關（管線排空）",                     0, 0, 300 },
        { "valid 滿 + credit 每拍回收（穩態滿吞吐）",   1, 1, 300 },
        { "valid 關 + credit 狂回收（credit 飽和）",     0, 1, 300 },
    };
    for (const auto &p : pats) {
        if (stop) break;
        for (int i = 0; i < p.n && !stop; i++)
            stop = !d.step(0, p.v, (uint8_t)(i & 63), (uint8_t)(i & 15), p.crd, 0);
    }
    /* rst 脈衝夾在流量中間（同時 flush + fill） */
    /* 同時 flush + fill：rst 與滿流量同一拍（需要 -r） */
    if (with_reset) {
        d.ctx.set_phase("directed-reset-under-traffic");
        for (int rep = 0; rep < 20 && !stop; rep++) {
            for (int i = 0; i < 10 && !stop; i++)
                stop = !d.step(0, 1, (uint8_t)(i * 7 & 63), (uint8_t)(i & 15), (uint8_t)(i & 1), lf.next());
            stop = !d.step(1, 1, 0x2A, 0xF, 1, lf.next());
            for (int i = 0; i < 10 && !stop; i++)
                stop = !d.step(0, 1, (uint8_t)(i & 63), (uint8_t)(i & 15), 1, lf.next());
        }
    }

    std::printf("活動量（覆蓋率代理）: in_ready 高 %ld 拍 (%.1f%%), "
                "out_valid 高 %ld 拍 (%.1f%%), out_tag 走過 %d/64 個值\n",
                d.n_ready, 100.0 * d.n_ready / (d.ctx.cycle ? d.ctx.cycle : 1),
                d.n_valid, 100.0 * d.n_valid / (d.ctx.cycle ? d.ctx.cycle : 1),
                __builtin_popcountll(d.tagseen));
    return d.ctx.report("equiv chain.v");
}
