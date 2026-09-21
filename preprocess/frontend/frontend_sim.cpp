/* ============================================================
 * frontend_sim.cpp — BSTM 離線前端模擬器（Agent A）
 *
 *   frontend_sim <instr-stream> <config.json> <out.fe> [stats.json]
 *
 * 讀一個文字指令串流，模擬完整前端預測階層
 *   uBTB (0 bubble) -> L2 BTB (1 bubble) -> TAGE 方向 (2 bubble)
 *   + RAS + wrong-path 展開
 * 每個 fetch block 輸出 1 byte overlay（格式見 include/bstf.h）
 * ============================================================ */

#include "../../include/bstf.h"
#include "json.hpp"
#include "predictors.hpp"
#include "sha256.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <ctime>

#define FE_SIM_VERSION "1.2"

using namespace fe;

/* ============================================================
 * 設定
 * ============================================================ */
struct Config {
    int  ubtb_entries    = 16;
    int  ubtb_tag_bits   = 20;
    int  btb_entries     = 8192;
    int  btb_ways        = 4;
    int  ubtb_slots      = 1;    /* uBTB 每個 entry 記幾個分支 */
    int  btb_slots       = 2;    /* L2 BTB 每個 entry 記幾個分支（FTB slot） */
    int  ras_depth       = 16;
    int  wrongpath_depth = 10;    /* D：誤預測後沿錯誤路徑模擬幾個 fetch block */
    int  fetch_width     = 4;     /* 每個 fetch block 最多幾條指令 */
    int  ilen_bytes      = 4;     /* 無法從 trace 推斷時的指令長度 */
    int  max_blk_instr   = 8;     /* 保險上限（trace 沒給 block_end 時） */
    /* wrong-path fetch 事件的輸出方式：
     *   0 = inline   夾在觸發它的 correct-path byte 後面（同一個 .fe）
     *   1 = separate 另外寫到 <out>.wp，主 .fe 維持純 correct-path（線性游標）
     *   2 = none     不輸出
     * 見 README §6.1 / §9：lib/runtime/bstf_reader.h 的 fe 游標是
     * 「correct-path 記錄的 fe_index_delta 累加」，inline 會讓它錯位。 */
    int  wp_out_mode     = 1;   /* 監督者 A1 裁決：預設 separate */
    bool emit_wrongpath  = true;  /* 舊名：0 等同 wrongpath_out="none" */
    bool btb_dir_hint    = true;  /* 1: BTB 有自己的 2-bit 方向 hint（TAGE 可再 override，2 bubble）
                                     0: BTB 階段直接用 TAGE 方向（最多只會有 1 bubble） */
    /* 監督者裁決：兩個 knob 對應兩種不同的硬體行為，預設值因此不同
     *   BTB/uBTB 配置發生在 fetch/predecode —— wrong path 的 fetch 真的跑過 → 預設 1
     *   TAGE 計數器多半在 retire 才更新 —— wrong-path 指令不會 retire → 預設 0
     * （全域歷史仍然推測更新 + 誤預測時精確還原，那是歷史不是計數器） */
    bool wp_train_btb    = true;   /* wrong-path 是否配置 uBTB/BTB entry */
    bool wp_train_tage   = false;  /* wrong-path 是否訓練 TAGE 計數器（代理假設，預設關） */
    bool allow_ambiguous_6col = false;  /* 6 欄且 not-taken target 非 0 時是否放行 */
    bool learn_call_len  = true;   /* 從 ret 的實際回傳位址回推 call 指令長度（RVC 2/4 byte） */
    TageCfg tage;

    int block_bytes() const { return fetch_width * ilen_bytes; }
};

static void default_tage(TageCfg& t) {
    t.bimodal_index_bits  = 13;
    t.bimodal_ctr_bits    = 2;
    t.useful_reset_period = 256 * 1024;
    t.use_alt_on_na       = true;
    const int hl[4] = {8, 16, 32, 64};
    t.tables.clear();
    for (int i = 0; i < 4; i++) {
        TageTableCfg c;
        c.hist_len = hl[i];
        c.entries  = 1024;
        c.tag_bits = 8;
        c.ctr_bits = 3;
        c.u_bits   = 2;
        t.tables.push_back(c);
    }
}

static Config load_config(const std::string& text) {
    Config c;
    default_tage(c.tage);
    mjson::Value v = mjson::Parser(text).parse();
    if (v.type != mjson::Value::OBJ) {
        fprintf(stderr, "frontend_sim: config root must be a JSON object\n");
        exit(2);
    }
    c.ubtb_entries    = (int)v.geti("ubtb_entries",    c.ubtb_entries);
    c.ubtb_tag_bits   = (int)v.geti("ubtb_tag_bits",   c.ubtb_tag_bits);
    c.btb_entries     = (int)v.geti("btb_entries",     c.btb_entries);
    c.btb_ways        = (int)v.geti("btb_ways",        c.btb_ways);
    c.ubtb_slots      = clampi((int)v.geti("ubtb_branch_slots", c.ubtb_slots), 1, FE_MAX_SLOTS);
    c.btb_slots       = clampi((int)v.geti("btb_branch_slots",  c.btb_slots),  1, FE_MAX_SLOTS);
    c.ras_depth       = (int)v.geti("ras_depth",       c.ras_depth);
    c.wrongpath_depth = (int)v.geti("wrongpath_depth", c.wrongpath_depth);
    c.fetch_width     = (int)v.geti("fetch_width",     c.fetch_width);
    c.ilen_bytes      = (int)v.geti("ilen_bytes",      c.ilen_bytes);
    c.max_blk_instr   = (int)v.geti("max_blk_instr",   c.max_blk_instr);
    c.emit_wrongpath  = v.geti("emit_wrongpath",  c.emit_wrongpath  ? 1 : 0) != 0;
    if (!c.emit_wrongpath) c.wp_out_mode = 2;
    if (v.has("wrongpath_out")) {
        std::string m = v["wrongpath_out"].as_str("inline");
        if      (m == "inline")   c.wp_out_mode = 0;
        else if (m == "separate") c.wp_out_mode = 1;
        else if (m == "none")     c.wp_out_mode = 2;
        else { fprintf(stderr, "frontend_sim: 不認識的 wrongpath_out \"%s\"\n", m.c_str()); exit(2); }
    }
    c.emit_wrongpath = (c.wp_out_mode != 2);
    c.btb_dir_hint    = v.geti("btb_dir_hint",    c.btb_dir_hint    ? 1 : 0) != 0;
    c.learn_call_len  = v.geti("learn_call_len",  c.learn_call_len  ? 1 : 0) != 0;
    c.allow_ambiguous_6col = v.geti("allow_ambiguous_6col", c.allow_ambiguous_6col ? 1 : 0) != 0;
    c.wp_train_btb    = v.geti("wrongpath_train_btb",  c.wp_train_btb  ? 1 : 0) != 0;
    c.wp_train_tage   = v.geti("wrongpath_train_tage", c.wp_train_tage ? 1 : 0) != 0;

    c.tage.bimodal_index_bits  = (int)v.geti("bimodal_index_bits",  c.tage.bimodal_index_bits);
    c.tage.bimodal_ctr_bits    = (int)v.geti("bimodal_ctr_bits",    c.tage.bimodal_ctr_bits);
    c.tage.useful_reset_period =       v.geti("useful_reset_period", c.tage.useful_reset_period);
    c.tage.use_alt_on_na       = v.geti("use_alt_on_na", c.tage.use_alt_on_na ? 1 : 0) != 0;

    /* tage_tables 可以是
     *   4                                        → 用預設 hist {8,16,32,64}
     *   [8,16,32,64]                             → 只給 history length
     *   [{"hist":8,"entries":1024,...}, ...]     → 完整描述
     */
    if (v.has("tage_tables")) {
        const mjson::Value& tt = v["tage_tables"];
        if (tt.type == mjson::Value::NUM) {
            int n = (int)tt.num;
            if (n >= 0 && n < (int)c.tage.tables.size()) c.tage.tables.resize(n);
            while ((int)c.tage.tables.size() < n) {   /* 幾何級數延長歷史 */
                TageTableCfg nc = c.tage.tables.empty() ? TageTableCfg() : c.tage.tables.back();
                nc.hist_len = c.tage.tables.empty() ? 8 : c.tage.tables.back().hist_len * 2;
                c.tage.tables.push_back(nc);
            }
        } else if (tt.type == mjson::Value::ARR) {
            std::vector<TageTableCfg> nt;
            for (const auto& e : tt.arr) {
                TageTableCfg tc;
                if (e.type == mjson::Value::NUM) {
                    tc.hist_len = (int)e.num;
                } else if (e.type == mjson::Value::OBJ) {
                    tc.hist_len = (int)e.geti("hist",     e.geti("hist_len", 8));
                    tc.entries  = (int)e.geti("entries",  1024);
                    tc.tag_bits = (int)e.geti("tag_bits", 8);
                    tc.ctr_bits = (int)e.geti("ctr_bits", 3);
                    tc.u_bits   = (int)e.geti("u_bits",   2);
                }
                nt.push_back(tc);
            }
            c.tage.tables = nt;   /* 空陣列 = 純 bimodal（對照組） */
        }
    }
    if (v.has("tage_entries"))
        for (auto& t : c.tage.tables) t.entries = (int)v.geti("tage_entries", t.entries);
    if (v.has("tage_tag_bits"))
        for (auto& t : c.tage.tables) t.tag_bits = (int)v.geti("tage_tag_bits", t.tag_bits);

    /* sanity */
    if (c.tage.tables.size() > 8) c.tage.tables.resize(8);   /* Tage::Pred 的固定陣列上限 */
    for (auto& t : c.tage.tables) {
        if (t.hist_len < 1)   t.hist_len = 1;
        if (t.hist_len > 500) t.hist_len = 500;              /* HistReg = 512 bit */
        if (t.entries  < 2)   t.entries  = 2;
        t.entries = 1 << log2ceil((uint64_t)t.entries);
        t.tag_bits = clampi(t.tag_bits, 4, 16);
        t.ctr_bits = clampi(t.ctr_bits, 2, 8);
        t.u_bits   = clampi(t.u_bits,   1, 4);
    }
    if (c.fetch_width   < 1) c.fetch_width   = 1;
    if (c.ilen_bytes    < 1) c.ilen_bytes    = 1;
    if (c.max_blk_instr < c.fetch_width) c.max_blk_instr = c.fetch_width;
    if (c.wrongpath_depth < 0) c.wrongpath_depth = 0;
    return c;
}

/* 有效配置的正規化字串（套完預設值），當成 sweep 的指紋 */
static std::string effective_config_string(const Config& c) {
    char b[512];
    std::string s;
    auto add = [&](const char* fmt, long v) { snprintf(b, sizeof b, fmt, v); s += b; };
    add("ubtb_entries=%ld\n",    c.ubtb_entries);
    add("ubtb_tag_bits=%ld\n",   c.ubtb_tag_bits);
    add("btb_entries=%ld\n",     c.btb_entries);
    add("btb_ways=%ld\n",        c.btb_ways);
    add("ubtb_branch_slots=%ld\n", c.ubtb_slots);
    add("btb_branch_slots=%ld\n",  c.btb_slots);
    add("btb_dir_hint=%ld\n",    c.btb_dir_hint ? 1 : 0);
    add("ras_depth=%ld\n",       c.ras_depth);
    add("wrongpath_depth=%ld\n", c.wrongpath_depth);
    add("wrongpath_out=%ld\n",   c.wp_out_mode);
    add("wrongpath_train_btb=%ld\n",  c.wp_train_btb  ? 1 : 0);
    add("wrongpath_train_tage=%ld\n", c.wp_train_tage ? 1 : 0);
    add("fetch_width=%ld\n",     c.fetch_width);
    add("ilen_bytes=%ld\n",      c.ilen_bytes);
    add("max_blk_instr=%ld\n",   c.max_blk_instr);
    add("bimodal_index_bits=%ld\n", c.tage.bimodal_index_bits);
    add("bimodal_ctr_bits=%ld\n",   c.tage.bimodal_ctr_bits);
    add("useful_reset_period=%ld\n", c.tage.useful_reset_period);
    add("use_alt_on_na=%ld\n",   c.tage.use_alt_on_na ? 1 : 0);
    for (size_t i = 0; i < c.tage.tables.size(); i++) {
        const TageTableCfg& t = c.tage.tables[i];
        snprintf(b, sizeof b, "tage[%zu]=hist:%d,entries:%d,tag:%d,ctr:%d,u:%d\n",
                 i, t.hist_len, t.entries, t.tag_bits, t.ctr_bits, t.u_bits);
        s += b;
    }
    return s;
}

/* ============================================================
 * 指令串流 → fetch block
 *
 * frontend format v2（7 欄，主路徑）:
 *   <pc_hex> <is_branch> <br_type> <taken> <target_hex> <is_block_end> <next_pc_hex>
 *   target_hex  = 解碼出的 architectural 目標，與 taken 無關，非分支為 0
 *   next_pc_hex = 下一條實際執行指令的 PC，最後一行為 0
 *
 * 6 欄舊格式有兩種慣例，必須分辨（見 §3）：
 *   (a) not-taken 的 target 恆為 0  -> gen_trace.py 舊慣例，用 lookahead 推導
 *   (b) not-taken 的 target 非 0    -> Agent H 舊檔案，缺 next_pc 又無指令長度，
 *                                      無法可靠推導 fall-through -> 直接報錯
 * ============================================================ */
struct Blk {
    uint64_t pc         = 0;
    uint64_t last_pc    = 0;
    uint32_t n_instr    = 0;
    BlkObs   obs;                 /* 這次執行看到的分支與 fall-through */
    bool     taken      = false;  /* block 以 taken branch 結束        */
    uint64_t next_pc    = 0;      /* ground truth 的下一個 block PC    */
    bool     has_next   = false;
    uint64_t ilen_guess = 4;

    uint64_t actual_next() const {
        if (has_next) return next_pc;
        if (taken && obs.taken_idx >= 0) return obs.br[obs.taken_idx].target;
        return obs.fallthru;
    }
};

static inline uint64_t parse_hex(const char* s, const char** end) {
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint64_t v = 0;
    const char* st = s;
    while (*s) {
        char c = *s;
        int  d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint64_t)d;
        s++;
    }
    *end = (s == st) ? st : s;
    return v;
}
static inline long parse_dec(const char* s, const char** end) {
    while (*s == ' ' || *s == '\t') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    *end = s;
    return neg ? -v : v;
}

class BlockReader {
public:
    BlockReader(std::istream& in, const Config& cfg) : in_(in), cfg_(cfg) {}

    uint64_t instr_count() const { return n_instr_; }
    uint64_t bad_lines()   const { return bad_; }
    bool     seen_7col()   const { return col7_; }
    uint64_t nt_zero_tgt() const { return nt_zero_; }
    uint64_t nt_nz_tgt()   const { return nt_nz_; }

    bool next(Blk& out) {
        if (!have_) {
            if (!read_raw(cur_)) return false;
            have_ = true;
        }
        out = cur_;
        Blk nx;
        if (read_raw(nx)) {
            /* 6 欄舊格式：下一個 block 的起始 PC 就是 next_pc */
            if (!out.has_next) { out.next_pc = nx.pc; out.has_next = true; }
            cur_  = nx;
            have_ = true;
        } else {
            have_ = false;
        }
        return true;
    }

private:
    std::istream& in_;
    const Config& cfg_;
    Blk           cur_;
    bool          have_    = false;
    bool          col7_    = false;
    uint64_t      n_instr_ = 0;
    uint64_t      bad_     = 0;
    uint64_t      nt_zero_ = 0;   /* not-taken 且 target == 0 的分支數 */
    uint64_t      nt_nz_   = 0;   /* not-taken 且 target != 0 的分支數 */
    std::string   line_;

    bool read_raw(Blk& b) {
        b = Blk();
        uint64_t last = 0, prev = 0;
        bool     any  = false;
        while (std::getline(in_, line_)) {
            const char* p = line_.c_str();
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '#' || *p == '\r') continue;
            const char* e;
            uint64_t pc = parse_hex(p, &e);
            if (e == p) { bad_++; continue; }
            p = e;
            long is_br = parse_dec(p, &e); p = e;
            long typ   = parse_dec(p, &e); p = e;
            long tkn   = parse_dec(p, &e); p = e;
            uint64_t tgt = parse_hex(p, &e); p = e;
            long bend  = parse_dec(p, &e); p = e;
            const char* p7 = p;
            uint64_t nxt  = parse_hex(p, &e);
            bool     has7 = (e != p7);
            p = e;
            if (has7) col7_ = true;

            if (!any) { b.pc = pc; any = true; last = pc; prev = pc; }
            else      { prev = last; last = pc; }
            b.n_instr++;
            n_instr_++;

            bool br  = is_br != 0;
            bool tak = br && (tkn != 0);
            if (br) {
                if (!tak) { if (tgt == 0) nt_zero_++; else nt_nz_++; }
                if (b.obs.n_br < FE_MAX_SLOTS) {
                    BrSlot& sl = b.obs.br[b.obs.n_br];
                    sl.pc           = pc;
                    sl.type         = (uint8_t)typ;
                    sl.valid        = true;
                    sl.target       = tgt;
                    sl.target_valid = (tgt != 0);
                    if (tak) b.obs.taken_idx = b.obs.n_br;
                }
                b.obs.n_br++;
            }
            b.taken    = tak;             /* 只有最後一條指令可能是 taken */
            b.next_pc  = has7 ? nxt : 0;
            b.has_next = has7 && nxt != 0;

            bool blk_end = (bend != 0) || tak || ((int)b.n_instr >= cfg_.max_blk_instr);
            if (blk_end) break;
        }
        if (!any) return false;
        b.last_pc = last;
        uint64_t ilen = (b.n_instr >= 2 && last > prev) ? (last - prev) : (uint64_t)cfg_.ilen_bytes;
        if (ilen == 0 || ilen > 8) ilen = (uint64_t)cfg_.ilen_bytes;
        b.ilen_guess   = ilen;
        b.obs.complete = !b.taken;        /* 沒有 taken -> 這次看到了整個 block */
        return true;
    }
};

/* ============================================================
 * 統計
 * ============================================================ */
struct Stats {
    uint64_t fe_blocks = 0, cp_blocks = 0, wp_blocks = 0, instrs = 0;
    uint64_t branch_blocks = 0, cond_branches = 0, uncond_branches = 0, total_branches = 0;
    uint64_t multi_br_blocks = 0, slot_overflow_blocks = 0;
    uint64_t mispred = 0, dir_mispred = 0, tgt_mispred = 0, cond_dir_mispred = 0;
    uint64_t ubtb_hit = 0, btb_hit = 0, btb_override = 0, tage_override = 0;
    uint64_t ras_pred = 0, ras_ok = 0;
    uint64_t bubbles_cp = 0, bubbles_all = 0;
    uint64_t bub_hist[4] = {0, 0, 0, 0};
    uint64_t wp_ubtb_hit = 0, wp_btb_hit = 0, wp_synthetic = 0, wp_bytes = 0;
    uint64_t learned_ilen = 0, learned_call_len = 0;
    int      trace_cols = 6;
    static const uint64_t CHUNK = 2048;
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    void chunk_tick(bool mis) {
        if (chunks.empty() || chunks.back().first >= CHUNK) chunks.push_back({0, 0});
        chunks.back().first++;
        if (mis) chunks.back().second++;
    }
};

/* ============================================================
 * 前端模擬器
 * ============================================================ */
struct FePred {
    bool     ubtb_hit = false, btb_hit = false, btb_override = false, tage_override = false;
    int      bubbles  = 0;
    bool     taken    = false;
    uint64_t next     = 0;
    uint64_t fallthru = 0;
    int      br_type  = BR_NONE;
    int      taken_idx = -1;
    /* 每個 BTB slot 的分支 PC 與最終預測方向（給 per-branch 統計用） */
    int      n_slot = 0;
    uint64_t slot_pc[FE_MAX_SLOTS]  = {0, 0, 0, 0};
    bool     slot_dir[FE_MAX_SLOTS] = {false, false, false, false};
};

class FrontendSim {
public:
    FrontendSim(const Config& cfg, FILE* fe_out, FILE* wp_out)
        : cfg_(cfg), out_(fe_out), wp_out_(wp_out) {
        ubtb_.init(cfg.ubtb_entries, cfg.ubtb_tag_bits, cfg.ubtb_slots);
        btb_.init(cfg.btb_entries, cfg.btb_ways, cfg.btb_slots);
        ras_.init(cfg.ras_depth);
        tage_.init(cfg.tage);
        buf_.reserve(1 << 16);
    }

    Stats& stats() { return st_; }

    void run(BlockReader& rd) {
        Blk b;
        while (rd.next(b)) {
            check_trace_format(rd);
            fix_fallthru(b);
            FePred p = predict(b.pc);

            bool dir_ok = (p.taken == b.taken);
            bool tgt_ok = (p.next  == b.actual_next());
            bool mis    = !(dir_ok && tgt_ok);

            uint8_t byte = (uint8_t)(p.bubbles & 0x3);
            if (p.ubtb_hit)     byte |= FE_UBTB_HIT;
            if (p.btb_override) byte |= FE_BTB_OVERRIDE;
            if (dir_ok)         byte |= FE_DIR_OK;
            if (tgt_ok)         byte |= FE_TGT_OK;
            if (mis)            byte |= FE_REDIRECT;
            emit(byte);

            st_.cp_blocks++;
            st_.instrs += b.n_instr;
            st_.bubbles_cp += (uint64_t)p.bubbles;
            st_.bub_hist[p.bubbles & 3]++;
            if (p.ubtb_hit)      st_.ubtb_hit++;
            if (p.btb_hit)       st_.btb_hit++;
            if (p.btb_override)  st_.btb_override++;
            if (p.tage_override) st_.tage_override++;
            if (b.obs.n_br > 0)  st_.branch_blocks++;
            if (b.obs.n_br > 1)  st_.multi_br_blocks++;
            if (b.obs.n_br > cfg_.btb_slots) st_.slot_overflow_blocks++;
            if (!dir_ok) st_.dir_mispred++;
            if (dir_ok && !tgt_ok) st_.tgt_mispred++;
            if (mis) st_.mispred++;
            st_.chunk_tick(mis);

            if (mis && cfg_.wrongpath_depth > 0) {
                Tage::Snapshot hs = tage_.save();
                Ras::Snapshot  rs = ras_.save();
                if (p.taken_idx >= 0 && p.br_type == BR_COND) tage_.push_history(true);
                if      (p.br_type == BR_CALL) ras_.push(p.fallthru, b.pc);
                else if (p.br_type == BR_RET)  ras_.pop();
                uint64_t wpc = p.next;
                for (int d = 0; d < cfg_.wrongpath_depth; d++) wpc = wrongpath_block(wpc);
                tage_.restore(hs);
                ras_.restore(rs);
            }

            resolve(b, p);

            StaticBlk& sb = smap_[b.pc];
            sb.seen = true;
            merge_static(sb.obs, b.obs);
        }
        st_.trace_cols   = rd.seen_7col() ? 7 : 6;
        st_.learned_ilen = (uint64_t)ilen_.size();
        flush();
    }

private:
    struct StaticBlk { BlkObs obs; bool seen = false; };

    Config cfg_;
    FILE*  out_;
    FILE*  wp_out_ = nullptr;
    UBtb   ubtb_;
    Btb    btb_;
    Ras    ras_;
    Tage   tage_;
    Stats  st_;
    std::unordered_map<uint64_t, StaticBlk> smap_;
    std::unordered_map<uint64_t, uint8_t>   ilen_;
    std::vector<uint8_t> buf_, wbuf_;

    /* 6 欄舊格式的兩種慣例必須分辨（見檔頭說明）。
     * not-taken 的 target 非 0 又沒有第 7 欄 = 缺指令長度，無法推 fall-through。*/
    void check_trace_format(const BlockReader& rd) {
        if (fmt_checked_) return;
        if (rd.seen_7col()) { fmt_checked_ = true; return; }
        if (rd.nt_nz_tgt() == 0) return;              /* 還沒看到證據，繼續看 */
        fmt_checked_ = true;
        fprintf(stderr,
            "frontend_sim: 錯誤 — 這是 6 欄的 trace，但 not-taken 分支的 target 非 0\n"
            "  (architectural target 慣例)。缺少第 7 欄 next_pc 且沒有指令長度資訊，\n"
            "  RVC 下無法可靠推導 fall-through。請改用 frontend format v2（7 欄）。\n"
            "  若確定要用近似值，設 allow_ambiguous_6col: 1。\n");
        if (!cfg_.allow_ambiguous_6col) exit(3);
        fprintf(stderr, "frontend_sim: allow_ambiguous_6col=1，改用推導的 fall-through 繼續。\n");
    }
    bool fmt_checked_ = false;

    void emit(uint8_t b) {
        st_.fe_blocks++;
        st_.bubbles_all += (uint64_t)FE_BUBBLES(b);
        buf_.push_back(b);
        if (buf_.size() >= (1u << 16)) flush();
    }
    void emit_wp(uint8_t b) {
        if (cfg_.wp_out_mode == 0) { emit(b); return; }
        if (cfg_.wp_out_mode == 2) return;
        st_.wp_bytes++;
        wbuf_.push_back(b);
        if (wbuf_.size() >= (1u << 16)) flush();
    }
    void flush() {
        if (!buf_.empty()  && out_)    fwrite(buf_.data(),  1, buf_.size(),  out_);
        if (!wbuf_.empty() && wp_out_) fwrite(wbuf_.data(), 1, wbuf_.size(), wp_out_);
        buf_.clear();
        wbuf_.clear();
    }

    static void merge_static(BlkObs& dst, const BlkObs& o) {
        int seen = o.n_br < FE_MAX_SLOTS ? o.n_br : FE_MAX_SLOTS;
        for (int i = 0; i < seen; i++) dst.br[i] = o.br[i];
        if (o.complete) {
            for (int i = seen; i < FE_MAX_SLOTS; i++) dst.br[i] = BrSlot();
            dst.n_br = o.n_br;
        } else if (o.n_br > dst.n_br) {
            dst.n_br = o.n_br;
        }
        dst.taken_idx = o.taken_idx;
        if (o.fallthru_valid) { dst.fallthru = o.fallthru; dst.fallthru_valid = true; }
    }

    /* block 的 fall-through：
     *   沒有 taken branch 且有第 7 欄 -> next_pc（精確，RVC 也對）
     *   否則 -> 這次執行看不到 block 的自然結尾，留給 BTB 之前學到的值 */
    void fix_fallthru(Blk& b) {
        if (!b.taken && b.has_next) {
            b.obs.fallthru       = b.next_pc;
            b.obs.fallthru_valid = true;
            return;
        }
        auto it = ilen_.find(b.last_pc);
        b.obs.fallthru       = b.last_pc + (it != ilen_.end() ? (uint64_t)it->second : b.ilen_guess);
        b.obs.fallthru_valid = false;     /* 只是猜的，不要寫進 FTB */
    }

    /* ---- 三階段預測 ---- */
    struct Sp { bool taken; uint64_t next; };
    static bool same(const Sp& a, const Sp& b) { return a.taken == b.taken && a.next == b.next; }

    /* 走 entry 的 slot，第一個被預測為 taken 的分支決定流向 */
    Sp eval(const FtbEntry* e, int nslots, bool use_tage, const Tage::Pred* tp,
            uint64_t seqfall, int& tidx, int& btype, uint64_t& fall) {
        Sp r;
        tidx  = -1;
        btype = BR_NONE;
        fall  = (e && e->fallthru_valid) ? e->fallthru : seqfall;
        if (!e) { r.taken = false; r.next = seqfall; return r; }
        for (int i = 0; i < nslots; i++) {
            if (!e->br[i].valid) continue;
            int  ty  = e->br[i].type;
            bool dir = br_is_uncond(ty) ? true
                                        : (use_tage ? tp[i].taken : (e->hint[i] >= 2));
            if (dir) {
                tidx    = i;
                btype   = ty;
                r.taken = true;
                r.next  = (ty == BR_RET) ? ras_.peek() : e->br[i].target;
                return r;
            }
        }
        r.taken = false;
        r.next  = fall;
        return r;
    }

    FePred predict(uint64_t pc) {
        FePred   p;
        uint64_t seqfall = pc + (uint64_t)cfg_.block_bytes();
        int      ti, bt;
        uint64_t fall;

        /* stage 1 : uBTB，0 bubble */
        const FtbEntry* ue = ubtb_.lookup(pc);
        if (ue) p.ubtb_hit = true;
        Tage::Pred dummy[FE_MAX_SLOTS];
        Sp s1 = eval(ue, ubtb_.slots(), false, dummy, seqfall, ti, bt, fall);

        /* stage 2 : L2 BTB（+RAS），1 bubble */
        const FtbEntry* be = btb_.lookup(pc);
        if (be) p.btb_hit = true;
        Sp s2 = eval(be, btb_.slots(), false, dummy, seqfall, ti, bt, fall);

        /* stage 3 : TAGE 方向，2 bubble。每個 cond slot 各查一次，共用同一份歷史 */
        Tage::Pred tp[FE_MAX_SLOTS];
        if (be) {
            for (int i = 0; i < btb_.slots(); i++)
                if (be->br[i].valid && be->br[i].type == BR_COND)
                    tp[i] = tage_.predict(be->br[i].pc);
        }
        Sp s3 = eval(be, btb_.slots(), true, tp, seqfall, ti, bt, fall);

        p.btb_override  = !same(s2, s1);
        p.tage_override = !same(s3, s2);
        p.bubbles       = p.tage_override ? 2 : (p.btb_override ? 1 : 0);
        p.taken         = s3.taken;
        p.next          = s3.next;
        p.fallthru      = fall;
        p.br_type       = bt;
        p.taken_idx     = ti;
        if (be) {
            p.n_slot = btb_.slots();
            for (int i = 0; i < btb_.slots(); i++) {
                p.slot_pc[i]  = be->br[i].valid ? be->br[i].pc : 0;
                p.slot_dir[i] = be->br[i].valid &&
                                (br_is_uncond(be->br[i].type) ? true : tp[i].taken);
            }
        }
        return p;
    }

    /* 這次預測時，對某個分支 PC 的方向預測是什麼（BTB 沒記到就是 not-taken） */
    static bool predicted_dir(const FePred& p, uint64_t brpc) {
        for (int i = 0; i < p.n_slot; i++)
            if (p.slot_pc[i] == brpc) return p.slot_dir[i];
        return false;
    }

    /* ---- 解析：用 ground truth 更新所有結構 ---- */
    void resolve(const Blk& b, const FePred& p) {
        /* 從 not-taken 的實際流向學到這條分支指令的長度（RVC 2 或 4 byte） */
        if (b.obs.n_br > 0 && !b.taken && b.has_next &&
            b.next_pc > b.last_pc && b.next_pc - b.last_pc <= 8)
            ilen_[b.last_pc] = (uint8_t)(b.next_pc - b.last_pc);

        /* FTB 更新：所有 fetch block 都要（包括完全沒有分支的 block） */
        ubtb_.update(b.pc, b.obs);
        btb_.update (b.pc, b.obs);

        /* 依序處理這次真的執行到的分支 */
        int nb   = b.obs.n_br < FE_MAX_SLOTS ? b.obs.n_br : FE_MAX_SLOTS;
        int last = (b.obs.taken_idx >= 0) ? b.obs.taken_idx : nb - 1;
        for (int i = 0; i <= last; i++) {
            const BrSlot& sl = b.obs.br[i];
            if (!sl.valid) continue;
            bool tk = (b.obs.taken_idx == i);
            st_.total_branches++;
            if (sl.type == BR_COND) {
                st_.cond_branches++;
                if (predicted_dir(p, sl.pc) != tk) st_.cond_dir_mispred++;
                Tage::Pred tp = tage_.predict(sl.pc);
                tage_.update(sl.pc, tk, tp);
                tage_.push_history(tk);
            } else {
                st_.uncond_branches++;
            }
            if (sl.type == BR_CALL) {
                auto it = ilen_.find(sl.pc);
                uint64_t ret_addr = sl.pc + (it != ilen_.end() ? (uint64_t)it->second
                                                               : (uint64_t)cfg_.ilen_bytes);
                ras_.push(ret_addr, sl.pc);
            } else if (sl.type == BR_RET) {
                Ras::Ent e   = ras_.pop();
                uint64_t act = b.actual_next();
                st_.ras_pred++;
                if (p.taken && p.br_type == BR_RET && p.next == act) st_.ras_ok++;
                /* call 是 taken，看不到它的 fall-through；用實際回傳位址回推長度 */
                if (cfg_.learn_call_len && e.valid && e.addr != act &&
                    act > e.call_pc && act - e.call_pc <= 8) {
                    ilen_[e.call_pc] = (uint8_t)(act - e.call_pc);
                    st_.learned_call_len++;
                }
            }
        }
    }

    /* ---- wrong path ---- */
    uint64_t wrongpath_block(uint64_t pc) {
        FePred p = predict(pc);

        auto it = smap_.find(pc);
        const StaticBlk* sb = (it == smap_.end()) ? nullptr : &it->second;

        bool     a_taken;
        uint64_t a_next;
        if (sb) {
            a_taken = sb->obs.taken_idx >= 0;
            a_next  = a_taken ? sb->obs.br[sb->obs.taken_idx].target : sb->obs.fallthru;
        } else {
            a_taken = false;
            a_next  = pc + (uint64_t)cfg_.block_bytes();
            st_.wp_synthetic++;
        }

        uint8_t byte = (uint8_t)(p.bubbles & 0x3) | FE_WRONGPATH;
        if (p.ubtb_hit)         byte |= FE_UBTB_HIT;
        if (p.btb_override)     byte |= FE_BTB_OVERRIDE;
        if (p.taken == a_taken) byte |= FE_DIR_OK;
        if (p.next  == a_next)  byte |= FE_TGT_OK;
        emit_wp(byte);

        st_.wp_blocks++;
        if (p.ubtb_hit) st_.wp_ubtb_hit++;
        if (p.btb_hit)  st_.wp_btb_hit++;

        if (sb) {
            if (cfg_.wp_train_tage) {
                int nb   = sb->obs.n_br < FE_MAX_SLOTS ? sb->obs.n_br : FE_MAX_SLOTS;
                int last = (sb->obs.taken_idx >= 0) ? sb->obs.taken_idx : nb - 1;
                for (int i = 0; i <= last; i++) {
                    const BrSlot& sl = sb->obs.br[i];
                    if (sl.valid && sl.type == BR_COND) {
                        Tage::Pred tp = tage_.predict(sl.pc);
                        tage_.update(sl.pc, sb->obs.taken_idx == i, tp);
                    }
                }
            }
            if (cfg_.wp_train_btb) {
                ubtb_.update(pc, sb->obs);
                btb_.update (pc, sb->obs);
            }
        }
        /* 推測歷史／RAS（走自己預測的那條路） */
        for (int i = 0; i < p.n_slot; i++) {
            if (!p.slot_pc[i]) continue;
            if (i > p.taken_idx && p.taken_idx >= 0) break;
        }
        if (p.br_type == BR_COND && p.taken) tage_.push_history(true);
        if      (p.br_type == BR_CALL) ras_.push(p.fallthru, pc);
        else if (p.br_type == BR_RET)  ras_.pop();

        return p.next;
    }
};

/* ============================================================
 * 統計輸出
 * ============================================================ */
static double rate(uint64_t a, uint64_t b) { return b ? (double)a / (double)b : 0.0; }

static void write_stats(const std::string& path, const Config& cfg, const Stats& st,
                        const std::string& trace, const std::string& cfgpath,
                        const std::string& fepath) {
    /* 十分位誤預測率 */
    std::vector<double> dec(10, 0.0);
    std::vector<uint64_t> db(10, 0), dm(10, 0);
    uint64_t total = st.cp_blocks;
    if (total) {
        uint64_t acc = 0;
        for (const auto& c : st.chunks) {
            int d = (int)((acc * 10) / total);
            if (d > 9) d = 9;
            db[d] += c.first;
            dm[d] += c.second;
            acc   += c.first;
        }
        for (int i = 0; i < 10; i++) dec[i] = rate(dm[i], db[i]);
    }

    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "frontend_sim: cannot write %s\n", path.c_str()); return; }
    fprintf(f, "{\n");
    fprintf(f, "  \"trace\": \"%s\",\n", trace.c_str());
    fprintf(f, "  \"config_file\": \"%s\",\n", cfgpath.c_str());
    fprintf(f, "  \"fe_file\": \"%s\",\n", fepath.c_str());
    fprintf(f, "  \"config\": {\n");
    fprintf(f, "    \"ubtb_entries\": %d,\n", cfg.ubtb_entries);
    fprintf(f, "    \"btb_entries\": %d,\n", cfg.btb_entries);
    fprintf(f, "    \"btb_ways\": %d,\n", cfg.btb_ways);
    fprintf(f, "    \"ubtb_branch_slots\": %d,\n", cfg.ubtb_slots);
    fprintf(f, "    \"btb_branch_slots\": %d,\n", cfg.btb_slots);
    fprintf(f, "    \"ras_depth\": %d,\n", cfg.ras_depth);
    fprintf(f, "    \"wrongpath_depth\": %d,\n", cfg.wrongpath_depth);
    fprintf(f, "    \"fetch_width\": %d,\n", cfg.fetch_width);
    fprintf(f, "    \"btb_dir_hint\": %d,\n", cfg.btb_dir_hint ? 1 : 0);
    fprintf(f, "    \"tage_tables\": [");
    for (size_t i = 0; i < cfg.tage.tables.size(); i++)
        fprintf(f, "%s{\"hist\":%d,\"entries\":%d,\"tag_bits\":%d,\"ctr_bits\":%d}",
                i ? ", " : "", cfg.tage.tables[i].hist_len, cfg.tage.tables[i].entries,
                cfg.tage.tables[i].tag_bits, cfg.tage.tables[i].ctr_bits);
    fprintf(f, "],\n");
    fprintf(f, "    \"bimodal_index_bits\": %d\n", cfg.tage.bimodal_index_bits);
    fprintf(f, "  },\n");
    fprintf(f, "  \"total_fe_blocks\": %llu,\n",   (unsigned long long)st.fe_blocks);
    fprintf(f, "  \"correct_path_blocks\": %llu,\n", (unsigned long long)st.cp_blocks);
    fprintf(f, "  \"wrongpath_blocks\": %llu,\n", (unsigned long long)st.wp_blocks);
    fprintf(f, "  \"wrongpath_synthetic_blocks\": %llu,\n", (unsigned long long)st.wp_synthetic);
    fprintf(f, "  \"wrongpath_out\": \"%s\",\n",
            cfg.wp_out_mode == 0 ? "inline" : (cfg.wp_out_mode == 1 ? "separate" : "none"));
    fprintf(f, "  \"wrongpath_file_bytes\": %llu,\n", (unsigned long long)st.wp_bytes);
    fprintf(f, "  \"instructions\": %llu,\n",      (unsigned long long)st.instrs);
    fprintf(f, "  \"trace_format_columns\": %d,\n", st.trace_cols);
    fprintf(f, "  \"learned_branch_lengths\": %llu,\n", (unsigned long long)st.learned_ilen);
    fprintf(f, "  \"learned_call_lengths\": %llu,\n",   (unsigned long long)st.learned_call_len);
    fprintf(f, "  \"branch_blocks\": %llu,\n",     (unsigned long long)st.branch_blocks);
    fprintf(f, "  \"multi_branch_blocks\": %llu,\n", (unsigned long long)st.multi_br_blocks);
    fprintf(f, "  \"slot_overflow_blocks\": %llu,\n",(unsigned long long)st.slot_overflow_blocks);
    fprintf(f, "  \"total_branches\": %llu,\n",     (unsigned long long)st.total_branches);
    fprintf(f, "  \"cond_branches\": %llu,\n",     (unsigned long long)st.cond_branches);
    fprintf(f, "  \"uncond_branches\": %llu,\n",   (unsigned long long)st.uncond_branches);
    fprintf(f, "  \"mispredicts\": %llu,\n",       (unsigned long long)st.mispred);
    fprintf(f, "  \"direction_mispredicts\": %llu,\n", (unsigned long long)st.dir_mispred);
    fprintf(f, "  \"target_only_mispredicts\": %llu,\n", (unsigned long long)st.tgt_mispred);
    fprintf(f, "  \"cond_dir_mispredicts\": %llu,\n", (unsigned long long)st.cond_dir_mispred);
    fprintf(f, "  \"mispredict_rate\": %.6f,\n",   rate(st.mispred, st.cp_blocks));
    fprintf(f, "  \"branch_mispredict_rate\": %.6f,\n", rate(st.mispred, st.branch_blocks));
    fprintf(f, "  \"cond_dir_mispredict_rate\": %.6f,\n", rate(st.cond_dir_mispred, st.cond_branches));
    fprintf(f, "  \"mpki\": %.4f,\n",              st.instrs ? 1000.0 * (double)st.mispred / (double)st.instrs : 0.0);
    fprintf(f, "  \"ubtb_hit_rate\": %.6f,\n",     rate(st.ubtb_hit, st.cp_blocks));
    fprintf(f, "  \"btb_hit_rate\": %.6f,\n",      rate(st.btb_hit, st.cp_blocks));
    fprintf(f, "  \"btb_override_rate\": %.6f,\n", rate(st.btb_override, st.cp_blocks));
    fprintf(f, "  \"tage_override_rate\": %.6f,\n",rate(st.tage_override, st.cp_blocks));
    fprintf(f, "  \"ras_predictions\": %llu,\n",   (unsigned long long)st.ras_pred);
    fprintf(f, "  \"ras_accuracy\": %.6f,\n",      rate(st.ras_ok, st.ras_pred));
    fprintf(f, "  \"avg_bubbles_per_block\": %.6f,\n", rate(st.bubbles_cp, st.cp_blocks));
    fprintf(f, "  \"avg_bubbles_per_block_all\": %.6f,\n", rate(st.bubbles_all, st.fe_blocks));
    fprintf(f, "  \"bubble_histogram\": [%llu, %llu, %llu, %llu],\n",
            (unsigned long long)st.bub_hist[0], (unsigned long long)st.bub_hist[1],
            (unsigned long long)st.bub_hist[2], (unsigned long long)st.bub_hist[3]);
    fprintf(f, "  \"wrongpath_ubtb_hit_rate\": %.6f,\n", rate(st.wp_ubtb_hit, st.wp_blocks));
    fprintf(f, "  \"mispredict_rate_by_decile\": [");
    for (int i = 0; i < 10; i++) fprintf(f, "%s%.6f", i ? ", " : "", dec[i]);
    fprintf(f, "]\n}\n");
    fclose(f);
}

/* sidecar：配置指紋與檔案關係，供 sweep / 回歸追溯（監督者 A5 裁決） */
static void write_meta(const std::string& path, const Config& cfg, const Stats& st,
                       const std::string& trace, const std::string& cfgpath,
                       const std::string& cfg_sha, size_t cfg_bytes,
                       const std::string& fepath, const std::string& wppath,
                       const std::string& eff, const std::string& eff_sha,
                       const std::string& stamp) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "frontend_sim: cannot write %s\n", path.c_str()); return; }
    const char* mode = cfg.wp_out_mode == 0 ? "inline" : (cfg.wp_out_mode == 1 ? "separate" : "none");
    fprintf(f, "{\n");
    fprintf(f, "  \"generator\": \"frontend_sim\",\n");
    fprintf(f, "  \"generator_version\": \"%s\",\n", FE_SIM_VERSION);
    fprintf(f, "  \"bstf_version\": %u,\n", (unsigned)BSTF_VERSION);
    fprintf(f, "  \"generated_utc\": \"%s\",\n", stamp.c_str());
    fprintf(f, "  \"trace\": \"%s\",\n", trace.c_str());
    fprintf(f, "  \"config_file\": \"%s\",\n", cfgpath.c_str());
    fprintf(f, "  \"config_bytes\": %zu,\n", cfg_bytes);
    fprintf(f, "  \"config_sha256\": \"%s\",\n", cfg_sha.c_str());
    fprintf(f, "  \"effective_config_sha256\": \"%s\",\n", eff_sha.c_str());
    fprintf(f, "  \"fe_file\": \"%s\",\n", fepath.c_str());
    fprintf(f, "  \"fe_bytes\": %llu,\n", (unsigned long long)st.fe_blocks);
    fprintf(f, "  \"n_fe_blocks\": %llu,\n", (unsigned long long)st.cp_blocks);
    fprintf(f, "  \"instructions\": %llu,\n", (unsigned long long)st.instrs);
    fprintf(f, "  \"wrongpath_out\": \"%s\",\n", mode);
    fprintf(f, "  \"wrongpath_depth\": %d,\n", cfg.wrongpath_depth);
    fprintf(f, "  \"wrongpath_file\": \"%s\",\n", wppath.c_str());
    fprintf(f, "  \"wrongpath_bytes\": %llu,\n", (unsigned long long)st.wp_bytes);
    fprintf(f, "  \"wrongpath_blocks\": %llu,\n", (unsigned long long)st.wp_blocks);
    fprintf(f, "  \"mispredicts\": %llu,\n", (unsigned long long)st.mispred);
    fprintf(f, "  \"wrongpath_layout\": \"第 k 次誤預測 -> byte 區間 [k*D, (k+1)*D)，D = wrongpath_depth\",\n");
    fprintf(f, "  \"effective_config\": \"");
    for (char ch : eff) {            /* 逐行 escape 成單一 JSON 字串 */
        if (ch == '\n')      fputs("\\n", f);
        else if (ch == '"')  fputs("\\\"", f);
        else if (ch == '\\') fputs("\\\\", f);
        else                 fputc(ch, f);
    }
    fprintf(f, "\"\n}\n");
    fclose(f);
}

static void print_stats(const Stats& st) {
    fprintf(stderr, "---- frontend_sim ----\n");
    fprintf(stderr, "fe blocks (total/correct/wrong) : %llu / %llu / %llu\n",
            (unsigned long long)st.fe_blocks, (unsigned long long)st.cp_blocks,
            (unsigned long long)st.wp_blocks);
    fprintf(stderr, "instructions                    : %llu  (trace 格式 %d 欄)\n",
            (unsigned long long)st.instrs, st.trace_cols);
    fprintf(stderr, "branch blocks / 分支數 / cond   : %llu / %llu / %llu  (多分支 block %llu)\n",
            (unsigned long long)st.branch_blocks, (unsigned long long)st.total_branches,
            (unsigned long long)st.cond_branches, (unsigned long long)st.multi_br_blocks);
    fprintf(stderr, "mispredicts                     : %llu  (%.4f%% of blocks, %.4f%% of branches)\n",
            (unsigned long long)st.mispred, 100.0 * rate(st.mispred, st.cp_blocks),
            100.0 * rate(st.mispred, st.branch_blocks));
    fprintf(stderr, "cond direction mispredict rate  : %.4f%%\n",
            100.0 * rate(st.cond_dir_mispred, st.cond_branches));
    fprintf(stderr, "MPKI                            : %.3f\n",
            st.instrs ? 1000.0 * (double)st.mispred / (double)st.instrs : 0.0);
    fprintf(stderr, "uBTB hit rate                   : %.4f%%\n", 100.0 * rate(st.ubtb_hit, st.cp_blocks));
    fprintf(stderr, "BTB  hit rate                   : %.4f%%\n", 100.0 * rate(st.btb_hit, st.cp_blocks));
    fprintf(stderr, "BTB  override rate              : %.4f%%\n", 100.0 * rate(st.btb_override, st.cp_blocks));
    fprintf(stderr, "TAGE override rate              : %.4f%%\n", 100.0 * rate(st.tage_override, st.cp_blocks));
    fprintf(stderr, "RAS accuracy                    : %.4f%% (%llu rets)\n",
            100.0 * rate(st.ras_ok, st.ras_pred), (unsigned long long)st.ras_pred);
    fprintf(stderr, "avg bubbles / block             : %.4f (correct path) %.4f (all)\n",
            rate(st.bubbles_cp, st.cp_blocks), rate(st.bubbles_all, st.fe_blocks));
    fprintf(stderr, "bubble histogram 0/1/2          : %llu / %llu / %llu\n",
            (unsigned long long)st.bub_hist[0], (unsigned long long)st.bub_hist[1],
            (unsigned long long)st.bub_hist[2]);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <instr-stream|-> <config.json> <out.fe> [stats.json]\n", argv[0]);
        return 2;
    }
    std::string trace = argv[1], cfgp = argv[2], outp = argv[3];
    std::string statp = (argc > 4) ? argv[4] : "";
    if (statp.empty()) {
        statp = outp;
        if (statp.size() > 3 && statp.compare(statp.size() - 3, 3, ".fe") == 0)
            statp.resize(statp.size() - 3);
        statp += ".stats.json";
    }

    /* 讀原始 config bytes（供 sha256 指紋），再解析 */
    std::string cfgtext;
    {
        std::ifstream cf(cfgp, std::ios::binary);
        if (!cf) { fprintf(stderr, "frontend_sim: cannot open %s\n", cfgp.c_str()); return 2; }
        std::ostringstream ss;
        ss << cf.rdbuf();
        cfgtext = ss.str();
    }
    Config cfg;
    try {
        cfg = load_config(cfgtext);
    } catch (const std::exception& e) {
        fprintf(stderr, "frontend_sim: %s\n", e.what());
        return 2;
    }

    std::ifstream fin;
    std::istream* in = &std::cin;
    if (trace != "-") {
        fin.open(trace);
        if (!fin) { fprintf(stderr, "frontend_sim: cannot open %s\n", trace.c_str()); return 2; }
        in = &fin;
    }
    FILE* fo = fopen(outp.c_str(), "wb");
    if (!fo) { fprintf(stderr, "frontend_sim: cannot open %s\n", outp.c_str()); return 2; }

    std::string wpp;
    FILE* fw = nullptr;
    if (cfg.wp_out_mode == 1) {
        wpp = outp + ".wp";
        fw  = fopen(wpp.c_str(), "wb");
        if (!fw) { fprintf(stderr, "frontend_sim: cannot open %s\n", wpp.c_str()); return 2; }
    }

    BlockReader rd(*in, cfg);
    FrontendSim sim(cfg, fo, fw);
    sim.run(rd);
    fclose(fo);
    if (fw) { fclose(fw); fprintf(stderr, "wrong-path overlay written to %s\n", wpp.c_str()); }

    print_stats(sim.stats());
    write_stats(statp, cfg, sim.stats(), trace, cfgp, outp);

    /* sidecar metadata */
    std::string base = outp;
    if (base.size() > 3 && base.compare(base.size() - 3, 3, ".fe") == 0) base.resize(base.size() - 3);
    std::string metap = base + ".fe.meta.json";
    std::string eff   = effective_config_string(cfg);
    char stamp[64] = "";
    {
        time_t t = time(nullptr);
        struct tm g;
        gmtime_r(&t, &g);
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &g);
    }
    write_meta(metap, cfg, sim.stats(), trace, cfgp,
               sha2::sha256_hex(cfgtext), cfgtext.size(), outp, wpp,
               eff, sha2::sha256_hex(eff), stamp);
    fprintf(stderr, "stats written to %s\n", statp.c_str());
    fprintf(stderr, "meta  written to %s\n", metap.c_str());
    return 0;
}
