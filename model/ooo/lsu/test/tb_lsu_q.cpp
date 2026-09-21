// ===================================================================
// lsu_q 功能自測（Agent F）
//   環境模型：fetch -> decode pipe -> dispatch -> be_eu 風格的 mem queue
//   檢查項（CONTRACT v7 §10）：
//     T1 每個記憶體 uop 恰好收到一次 done、不卡死
//     T2 mem-event side FIFO 對齊：實測延遲不得低於該 uop 的 base latency
//     T3 MSHR 是 MLP 上限：全 DRAM 時吞吐 <= MSHR_N / latency
//     T4 cnt_st_mshr 語意：全 L1 hit 時為 0、全 DRAM 時 > 0
//     T5 cfg_ldq/stq 在 最小/中間/MAX 三點都要前進，且吞吐不隨 mask 變大而劣化
// ===================================================================
#include "Vlsu_q.h"
#include "verilated.h"
#include "layout.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <deque>
#include <map>
#include <cstdint>

// ---- bstf_lat_table（include/bstf.h），測試端的期望值 ----
static const int LAT[16] = {3,4,12,16,30,40,90,120,160,200,2,8,50,70,140,255};
enum { UC_LOAD = 4, UC_STORE = 5, UC_AMO = 12, UC_ALU = 0 };

static int g_fail = 0;
static void CHECK(bool ok, const char* what, const char* ctx) {
    if (!ok) { printf("    FAIL  %s  [%s]\n", what, ctx); g_fail++; }
}

// ---- 寬訊號的位元寫入（verilator 對 >64 bit 的 port 用 WData 陣列）----
template <typename T>
static void wr_wide(T& sig, int lsb, int w, uint64_t val) {
    uint32_t* p = reinterpret_cast<uint32_t*>(&sig);
    for (int b = 0; b < w; b++) {
        int i = lsb + b, word = i >> 5, bit = i & 31;
        uint32_t m = 1u << bit;
        p[word] = (p[word] & ~m) | (((val >> b) & 1u) ? m : 0u);
    }
}
static void wr_nar(uint32_t& sig, int lsb, int w, uint32_t val) {
    for (int b = 0; b < w; b++) {
        uint32_t m = 1u << (lsb + b);
        sig = (sig & ~m) | (((val >> b) & 1u) ? m : 0u);
    }
}
static uint32_t rd_nar(uint32_t sig, int lsb, int w) {
    return (w >= 32) ? sig : ((sig >> lsb) & ((1u << w) - 1u));
}

struct Uop {
    int  cls;        // UC_*
    int  ev;         // mem event byte
    int  rob;
    int  prf;
    bool is_mem;
    bool is_miss;    // load/AMO 且 MEM_LEVEL != 0 -> 會佔 MSHR
    int  base_lat;   // 期望的 base latency（L1 hit / miss 都適用）
};

struct Result {
    long cycles = 0, dispatched = 0, done = 0, mshr_stall = 0;
    long miss_done = 0;       // 只算真的 miss（load/AMO 且 level!=0）
    long max_lat = 0;
    long lat_violations = 0;  // 實測延遲 < base latency 的次數
    bool hang = false;
};

// mix: 0 = 全 L1 hit, 1 = 全 DRAM miss, 2 = 混合（每 5 個一個 DRAM）
static Result run(int cfg_ldq, int cfg_stq, int mix, long max_cycles,
                  int n_uops, const char* ctx, int decode_delay = 2)
{
    Vlsu_q* dut = new Vlsu_q;
    Result R;

    // ---------------- 產生指令流（決定性 LCG）----------------
    std::vector<Uop> prog;
    uint32_t rnd = 0x1234567u;
    auto nxt = [&]() { rnd = rnd * 1103515245u + 12345u; return (rnd >> 16) & 0x7fff; };
    int rob_seq = 0;
    for (int i = 0; i < n_uops; i++) {
        Uop u{};
        int r = nxt() % 100;
        if      (r < 45) u.cls = UC_LOAD;
        else if (r < 70) u.cls = UC_STORE;
        else if (r < 72) u.cls = UC_AMO;
        else             u.cls = UC_ALU;
        u.is_mem = (u.cls == UC_LOAD || u.cls == UC_STORE || u.cls == UC_AMO);
        int lvl = 0, lc = 0;
        if (u.is_mem) {
            if (mix == 1)                 { lvl = 3; lc = 6; }   // DRAM, 90
            else if (mix == 2 && (i % 5) == 0) { lvl = 3; lc = 6; }
            else                          { lvl = 0; lc = 0; }   // L1 hit, 3
        }
        u.ev = (lc << 4) | lvl;
        // store 一律走 store buffer（ST_LAT=2），load/AMO 才查表
        u.base_lat = (u.cls == UC_STORE) ? 2 : LAT[lc];
        u.is_miss  = u.is_mem && (u.cls != UC_STORE) && (lvl != 0);
        u.rob = rob_seq % L_ROB_N;
        u.prf = (i * 7) % L_PRF_N;
        if (u.is_mem) rob_seq++;
        prog.push_back(u);
    }

    // ---------------- 環境狀態 ----------------
    std::deque<Uop>  decode_pipe;                 // fetch -> dispatch（模擬 DE/RN 延遲）
    std::deque<int>  eu_mq;                       // be_eu 風格的 mem request queue
    std::map<int, Uop> inflight;                  // rob -> uop（已 dispatch 未完成）
    std::map<int, long> req_cycle;                // rob -> dispatch 的 cycle
    std::vector<long> rob_free_at(L_ROB_N, 0);    // rob 重用的最早 cycle
    std::vector<int> pend_req;                    // 下一拍要驅動的 req
    size_t fetch_p = 0;
    const int DECODE_DELAY = decode_delay;
    std::deque<std::vector<Uop>> delay_line;

    dut->clk = 0; dut->rst = 1; dut->flush = 0;
    dut->cfg_ldq_entries = cfg_ldq; dut->cfg_stq_entries = cfg_stq;
    dut->ds_valid = 0; dut->req_v = 0; dut->fb_valid = 0; dut->fb_take = 0;
    memset(&dut->ds_ruop, 0, sizeof(dut->ds_ruop));
    memset(&dut->fb_duop, 0, sizeof(dut->fb_duop));
    dut->ds_robidx = 0; dut->req_rob = 0; dut->fb_mem_event = 0; dut->req_ev = 0;
    for (int i = 0; i < 4; i++) { dut->eval(); dut->clk = !dut->clk; }
    dut->rst = 0;

    long cyc = 0;
    long last_progress = 0;
    long done_total = 0;

    while (cyc < max_cycles) {
        // ================= 驅動輸入（clk 低電位）=================
        // ---- fetch 側 ----
        dut->fb_valid = 0; dut->fb_take = 0; dut->fb_mem_event = 0;
        memset(&dut->fb_duop, 0, sizeof(dut->fb_duop));
        std::vector<Uop> fetched;
        if (fetch_p < prog.size() && decode_pipe.size() + L_W <= 16) {  // = fe_front DQ_N
            int take = 0;
            for (int l = 0; l < L_W && fetch_p < prog.size(); l++, fetch_p++) {
                const Uop& u = prog[fetch_p];
                dut->fb_valid |= (1u << l);
                wr_wide(dut->fb_duop, l * L_DUOP_W + L_DUOP_CLASS_LSB,
                        L_DUOP_CLASS_W, u.cls);
                wr_nar(dut->fb_mem_event, l * 8, 8, u.ev);
                fetched.push_back(u);
                take++;
            }
            dut->fb_take = take;
        }

        // ---- dispatch 側（受 lsq_full 反壓；整組收）----
        dut->ds_valid = 0; dut->ds_robidx = 0;
        memset(&dut->ds_ruop, 0, sizeof(dut->ds_ruop));
        bool can_disp = !dut->lsq_full && decode_pipe.size() >= 1
                        && (int)inflight.size() + L_W < L_ROB_N - 4;
        std::vector<Uop> dispatched;
        if (can_disp) {
            for (int l = 0; l < L_W && !decode_pipe.empty(); l++) {
                Uop u = decode_pipe.front();
                // rob 必須在 in-flight 中唯一
                // rob 必須在 in-flight 中唯一，且與上次使用隔開足夠距離
                if (u.is_mem && (inflight.count(u.rob) || cyc < rob_free_at[u.rob])) break;
                decode_pipe.pop_front();
                dut->ds_valid |= (1u << l);
                wr_wide(dut->ds_ruop, l * L_RUOP_W + L_RUOP_CLASS_LSB,
                        L_RUOP_CLASS_W, u.cls);
                wr_wide(dut->ds_ruop, l * L_RUOP_W + L_RUOP_D_LSB,
                        L_RUOP_D_W, u.prf);
                wr_wide(dut->ds_ruop, l * L_RUOP_W + L_RUOP_DV_LSB, 1, 1);
                wr_nar(dut->ds_robidx, l * L_ROB_W, L_ROB_W, u.rob);
                dispatched.push_back(u);
            }
        }

        // ---- EU 側：把 req 驅動上去（上一拍決定的）----
        dut->req_v = 0; dut->req_rob = 0;
        for (size_t k = 0; k < pend_req.size(); k++) {
            dut->req_v |= (1u << k);
            wr_nar(dut->req_rob, k * L_ROB_W, L_ROB_W, pend_req[k]);
        }

        dut->eval();

        // ================= 取樣輸出 =================
        for (int l = 0; l < L_W; l++) {
            if (dut->done_v & (1u << l)) {
                int rb = rd_nar(dut->done_rob, l * L_ROB_W, L_ROB_W);
                auto it = inflight.find(rb);
                CHECK(it != inflight.end(), "done for unknown rob", ctx);
                if (it != inflight.end()) {
                    long lat = cyc - req_cycle[rb];
                    // T2：mem-event 對齊 —— 實測延遲不得低於 base latency
                    if (lat < it->second.base_lat) R.lat_violations++;
                    if (it->second.is_miss) R.miss_done++;
                    if (lat > R.max_lat) R.max_lat = lat;
                    inflight.erase(it);
                    req_cycle.erase(rb);
                    rob_free_at[rb] = cyc + 64;   // 等 DUT 把舊 entry 排掉
                    done_total++;
                    last_progress = cyc;
                }
            }
        }
        // be_eu 模型：lsu_ready 時一次 pop 最多 W 個（pop 決定暫存一拍）
        pend_req.clear();
        if (dut->lsu_ready) {
            for (int k = 0; k < L_W && !eu_mq.empty(); k++) {
                pend_req.push_back(eu_mq.front()); eu_mq.pop_front();
            }
        }

        // ================= 推進 clock =================
        dut->clk = 1; dut->eval();
        dut->clk = 0; dut->eval();

        // ---- 環境自己的 pipeline（req_cycle 與 done 取樣同基準）----
        delay_line.push_back(fetched);
        if ((int)delay_line.size() > DECODE_DELAY) {
            for (const Uop& u : delay_line.front()) decode_pipe.push_back(u);
            delay_line.pop_front();
        }
        for (const Uop& u : dispatched) {
            R.dispatched++;
            if (u.is_mem) {
                inflight[u.rob] = u;
                req_cycle[u.rob] = cyc;      // 保守起點，實際 req 更晚
                eu_mq.push_back(u.rob);
            }
        }

        size_t in_delay = 0;
        for (const auto& v : delay_line) in_delay += v.size();
        if (fetch_p >= prog.size() && inflight.empty() && decode_pipe.empty()
            && eu_mq.empty() && in_delay == 0 && pend_req.empty()) break;
        cyc++;
        if (cyc - last_progress > 5000) { R.hang = true; break; }
    }

    R.cycles = cyc;
    R.done = done_total;
    R.mshr_stall = dut->cnt_st_mshr;
    CHECK(!R.hang, "model hung (no completion for 5000 cycles)", ctx);
    CHECK(inflight.empty(), "some mem uops never completed", ctx);
    delete dut;
    return R;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    printf("  lsu_q 功能自測  (ROB_N=%d ROB_W=%d PRF_N=%d PRF_W=%d LDQ_N=%d LSQ_W=%d MSHR_N=%d)\n",
           L_ROB_N, L_ROB_W, L_PRF_N, L_PRF_W, L_LDQ_N, L_LSQ_W, L_MSHR_N);

    // ---- T5：cfg_ldq / cfg_stq 的 最小 / 中間 / MAX 三點 ----
    int pts[3] = { 1, L_LDQ_N / 2, L_LDQ_N };
    long thr[3];
    for (int i = 0; i < 3; i++) {
        char ctx[64]; snprintf(ctx, sizeof ctx, "cfg_ldq=cfg_stq=%d mix=hit", pts[i]);
        Result r = run(pts[i], pts[i], 0, 200000, 4000, ctx);
        thr[i] = r.done * 1000 / (r.cycles ? r.cycles : 1);
        printf("    cfg=%-2d  cycles=%-6ld done=%-5ld thr=%ld.%03ld/cyc  st_mshr=%ld  latviol=%ld\n",
               pts[i], r.cycles, r.done, thr[i] / 1000, thr[i] % 1000,
               r.mshr_stall, r.lat_violations);
        CHECK(r.done > 0, "no completions", ctx);
        CHECK(r.lat_violations == 0, "latency below base (mem-event misaligned?)", ctx);
        // T4a：全 L1 hit 不該有 MSHR stall
        CHECK(r.mshr_stall == 0, "cnt_st_mshr != 0 on all-L1-hit", ctx);
    }
    // 吞吐不得隨 mask 變大而劣化（CONTRACT v7 §10.3）
    CHECK(thr[1] >= thr[0], "throughput regressed cfg min -> mid", "monotonic");
    CHECK(thr[2] >= thr[1], "throughput regressed cfg mid -> MAX", "monotonic");

    // ---- T3 / T4b：全 DRAM，MSHR 是 MLP 上限 ----
    {
        const char* ctx = "cfg=MAX mix=all-DRAM";
        Result r = run(L_LDQ_N, L_STQ_N, 1, 400000, 1200, ctx);
        // 只有 miss 會佔 MSHR：store 走 store buffer、不配 MSHR（見 README §4.3）
        double miss_thr = (double)r.miss_done / (double)r.cycles;
        // MLP 上限 = min(MSHR_N, LDQ_N) / latency —— 兩者誰小誰就是瓶頸
        int    mlp_lim  = (L_MSHR_N < L_LDQ_N) ? L_MSHR_N : L_LDQ_N;
        double ceil_    = (double)mlp_lim / 90.0;       // 90 = LAT[6] (DRAM)
        printf("    all-DRAM cycles=%-7ld done=%-5ld miss=%-5ld miss_thr=%.4f/cyc"
               "  MLP ceiling=%.4f (lim=%d)  st_mshr=%ld  latviol=%ld\n",
               r.cycles, r.done, r.miss_done, miss_thr, ceil_, mlp_lim,
               r.mshr_stall, r.lat_violations);
        CHECK(miss_thr <= ceil_ * 1.02, "miss throughput exceeds MSHR MLP ceiling", ctx);
        // 只有在 LDQ 真的能撐出超過 MSHR_N 個未完成 miss 時，MSHR 才會是瓶頸；
        // LDQ_N <= MSHR_N 時 LDQ 先滿，cnt_st_mshr == 0 是正確行為。
        if (L_LDQ_N > L_MSHR_N)
            CHECK(r.mshr_stall > 0, "cnt_st_mshr == 0 under all-DRAM pressure", ctx);
        else
            CHECK(r.mshr_stall == 0, "cnt_st_mshr != 0 although LDQ is the limiter", ctx);
        CHECK(r.lat_violations == 0, "latency below base (mem-event misaligned?)", ctx);
    }

    // ---- T1 / T2：混合流（每 5 個一個 DRAM），最嚴格的對齊測試 ----
    {
        const char* ctx = "cfg=MAX mix=1-in-5-DRAM";
        Result r = run(L_LDQ_N, L_STQ_N, 2, 400000, 3000, ctx);
        printf("    mixed    cycles=%-7ld done=%-5ld miss=%-5ld max_lat=%ld"
               "  st_mshr=%ld  latviol=%ld\n",
               r.cycles, r.done, r.miss_done, r.max_lat, r.mshr_stall, r.lat_violations);
        CHECK(r.done > 0, "no completions", ctx);
        CHECK(r.lat_violations == 0, "latency below base (mem-event misaligned?)", ctx);
    }

    // ---- T6（負測）：刻意把 fetch->dispatch 距離拉到超過 mem-event FIFO 深度，
    //      證明 T2 這個檢查真的有牙齒（溢位會被抓到，不會靜默吃掉）----
    {
        Result r = run(L_LDQ_N, L_STQ_N, 2, 400000, 3000, "overflow-negative", 12);
        printf("    negative(deep fetch pipe) latviol=%ld  <- 應該 > 0\n", r.lat_violations);
        CHECK(r.lat_violations > 0,
              "T2 check has no teeth: FIFO overflow went undetected", "overflow-negative");
    }

    if (g_fail == 0) printf("  ALL PASS\n");
    else             printf("  %d FAILURE(S)\n", g_fail);
    return g_fail ? 1 : 0;
}
