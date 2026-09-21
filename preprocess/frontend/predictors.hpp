/* ============================================================
 * predictors.hpp — 前端預測結構：uBTB / L2 BTB / TAGE / RAS
 * BSTM Agent A / preprocess/frontend         C++17，無外部相依
 * ============================================================ */
#ifndef BSTM_PREDICTORS_HPP
#define BSTM_PREDICTORS_HPP

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>
#include <algorithm>

namespace fe {

/* ---- branch type，與輸入串流的 br_type 欄位一致 ---- */
enum BrType { BR_COND = 0, BR_JAL = 1, BR_JALR = 2, BR_RET = 3, BR_CALL = 4, BR_NONE = 7 };

static inline bool br_is_uncond(int t) { return t != BR_COND && t != BR_NONE; }

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline int log2ceil(uint64_t n) {
    int b = 0;
    while ((1ull << b) < n) b++;
    return b;
}

/* ============================================================
 * 全域歷史暫存器（512 bit 環狀左移），bit(0) = 最新
 * ============================================================ */
struct HistReg {
    static const int NW = 8;              /* 8 * 64 = 512 bit */
    uint64_t w[NW];

    void reset() { memset(w, 0, sizeof(w)); }
    int  bit(int i) const {
        if (i < 0 || i >= NW * 64) return 0;
        return (int)((w[i >> 6] >> (i & 63)) & 1ull);
    }
    void push(int taken) {
        for (int i = NW - 1; i > 0; i--) w[i] = (w[i] << 1) | (w[i - 1] >> 63);
        w[0] = (w[0] << 1) | (uint64_t)(taken & 1);
    }
};

/* ============================================================
 * 折疊歷史（Seznec CSR）：把 origLen 位歷史壓成 targetLen 位
 * 必須在 HistReg::push() 之後呼叫 update()
 * ============================================================ */
struct Folded {
    int      olen = 0, tlen = 1, outp = 0;
    uint32_t f = 0;

    void init(int o, int t) {
        olen = o;
        tlen = (t < 1) ? 1 : t;
        outp = olen % tlen;
        f    = 0;
    }
    void update(const HistReg& h) {
        f  = (f << 1) | (uint32_t)h.bit(0);
        f ^= (uint32_t)h.bit(olen) << outp;
        f ^= (f >> tlen);
        f &= (tlen >= 32) ? 0xFFFFFFFFu : ((1u << tlen) - 1u);
    }
};

/* ============================================================
 * TAGE：base bimodal + N 個 tagged component
 * ============================================================ */
struct TageTableCfg {
    int hist_len = 8;
    int entries  = 1024;
    int tag_bits = 8;
    int ctr_bits = 3;
    int u_bits   = 2;
};

struct TageCfg {
    int  bimodal_index_bits = 13;
    int  bimodal_ctr_bits   = 2;
    long useful_reset_period = 256 * 1024;   /* useful 老化週期（更新次數） */
    bool use_alt_on_na      = true;
    std::vector<TageTableCfg> tables;
};

class Tage {
public:
    struct Snapshot {                 /* wrong-path 回滾用（只含推測歷史） */
        HistReg              h;
        std::vector<uint32_t> fidx, ftag0, ftag1;
    };

    void init(const TageCfg& cfg) {
        cfg_ = cfg;
        nt_  = (int)cfg.tables.size();
        bim_bits_ = cfg.bimodal_index_bits;
        bim_.assign((size_t)1 << bim_bits_, (uint8_t)((1 << cfg.bimodal_ctr_bits) / 2)); /* weak taken */
        bim_max_ = (1 << cfg.bimodal_ctr_bits) - 1;
        bim_thr_ = (1 << cfg.bimodal_ctr_bits) / 2;

        t_.resize(nt_);
        hist_.reset();
        for (int i = 0; i < nt_; i++) {
            T& t      = t_[i];
            t.cfg     = cfg.tables[i];
            t.idx_bits = log2ceil((uint64_t)t.cfg.entries);
            t.ctr.assign((size_t)t.cfg.entries, 0);
            t.tag.assign((size_t)t.cfg.entries, 0);
            t.u.assign((size_t)t.cfg.entries, 0);
            t.ctr_max = (1 << (t.cfg.ctr_bits - 1)) - 1;
            t.ctr_min = -(1 << (t.cfg.ctr_bits - 1));
            t.u_max   = (1 << t.cfg.u_bits) - 1;
            t.fidx.init(t.cfg.hist_len, t.idx_bits);
            t.ftag[0].init(t.cfg.hist_len, t.cfg.tag_bits);
            t.ftag[1].init(t.cfg.hist_len, std::max(1, t.cfg.tag_bits - 1));
        }
        use_alt_ctr_ = 8;
        tick_        = 0;
        rng_         = 0x2545F4914F6CDD1Dull;
    }

    int  n_tables() const { return nt_; }

    /* 預測結果（也當成 update 的 scratch） */
    struct Pred {
        bool taken     = false;
        bool alt_taken = false;
        int  provider  = -1;   /* -1 = bimodal */
        int  altprov   = -1;
        bool used_alt  = false;
        uint32_t idx[8]  = {0};
        uint32_t tag[8]  = {0};
        uint32_t bimidx  = 0;
    };

    Pred predict(uint64_t pc) const {
        Pred p;
        p.bimidx = bim_index(pc);
        bool bim = bim_[p.bimidx] >= bim_thr_;
        for (int i = 0; i < nt_; i++) {
            p.idx[i] = index(i, pc);
            p.tag[i] = tag(i, pc);
            if (t_[i].tag[p.idx[i]] == p.tag[i]) {
                p.altprov  = p.provider;
                p.provider = i;
            }
        }
        if (p.provider < 0) {
            p.taken = p.alt_taken = bim;
            return p;
        }
        const T& pt = t_[p.provider];
        bool prov_pred = pt.ctr[p.idx[p.provider]] >= 0;
        bool alt_pred  = (p.altprov >= 0)
                       ? (t_[p.altprov].ctr[p.idx[p.altprov]] >= 0)
                       : bim;
        p.alt_taken = alt_pred;
        bool weak   = (pt.ctr[p.idx[p.provider]] == 0 || pt.ctr[p.idx[p.provider]] == -1);
        bool newly  = weak && pt.u[p.idx[p.provider]] == 0;
        if (cfg_.use_alt_on_na && newly && use_alt_ctr_ >= 8) {
            p.taken    = alt_pred;
            p.used_alt = true;
        } else {
            p.taken = prov_pred;
        }
        return p;
    }

    /* 解析後更新（只對 conditional branch 呼叫） */
    void update(uint64_t pc, bool taken, const Pred& p) {
        bool prov_pred;
        if (p.provider >= 0) {
            T& pt = t_[p.provider];
            uint32_t i = p.idx[p.provider];
            prov_pred  = pt.ctr[i] >= 0;
            bool weak  = (pt.ctr[i] == 0 || pt.ctr[i] == -1);
            bool newly = weak && pt.u[i] == 0;

            if (newly && prov_pred != p.alt_taken) {
                if (p.alt_taken == taken) use_alt_ctr_ = clampi(use_alt_ctr_ + 1, 0, 15);
                else                      use_alt_ctr_ = clampi(use_alt_ctr_ - 1, 0, 15);
            }
            if (prov_pred != p.alt_taken) {
                pt.u[i] = (uint8_t)clampi((int)pt.u[i] + (prov_pred == taken ? 1 : -1), 0, pt.u_max);
            }
            ctr_update(pt.ctr[i], taken, pt.ctr_min, pt.ctr_max);
            /* newly-allocated 時也一起訓練 altpred，加速收斂 */
            if (newly) {
                if (p.altprov >= 0) {
                    T& at = t_[p.altprov];
                    ctr_update(at.ctr[p.idx[p.altprov]], taken, at.ctr_min, at.ctr_max);
                } else {
                    bim_update(p.bimidx, taken);
                }
            }
        } else {
            prov_pred = bim_[p.bimidx] >= bim_thr_;
            bim_update(p.bimidx, taken);
        }

        /* --- allocate on mispredict --- */
        if (p.taken != taken && p.provider < nt_ - 1) {
            int start = p.provider + 1;
            int found = -1;
            for (int j = start; j < nt_; j++) {
                if (t_[j].u[p.idx[j]] == 0) { found = j; break; }
            }
            if (found < 0) {
                for (int j = start; j < nt_; j++)
                    if (t_[j].u[p.idx[j]] > 0) t_[j].u[p.idx[j]]--;
            } else {
                /* Seznec：在 found 與 found+1 之間偏好較短的表 */
                int j = found;
                if (found + 1 < nt_ && t_[found + 1].u[p.idx[found + 1]] == 0 && (rnd() & 3) == 0)
                    j = found + 1;
                T& at = t_[j];
                uint32_t i = p.idx[j];
                at.tag[i] = (uint16_t)p.tag[j];
                at.ctr[i] = taken ? 0 : -1;    /* weak */
                at.u[i]   = 0;
            }
        }

        /* --- useful 老化：每 period 次更新清掉 u 的一個 bit --- */
        tick_++;
        if (cfg_.useful_reset_period > 0 && (tick_ % cfg_.useful_reset_period) == 0) {
            uint8_t mask = ((tick_ / cfg_.useful_reset_period) & 1) ? 0x1 : 0x2;
            for (int i = 0; i < nt_; i++)
                for (size_t k = 0; k < t_[i].u.size(); k++)
                    t_[i].u[k] &= (uint8_t)~mask;
        }
    }

    /* 歷史推進（conditional branch 解析 / wrong-path 推測） */
    void push_history(bool taken) {
        hist_.push(taken ? 1 : 0);
        for (int i = 0; i < nt_; i++) {
            t_[i].fidx.update(hist_);
            t_[i].ftag[0].update(hist_);
            t_[i].ftag[1].update(hist_);
        }
    }

    Snapshot save() const {
        Snapshot s;
        s.h = hist_;
        s.fidx.resize(nt_); s.ftag0.resize(nt_); s.ftag1.resize(nt_);
        for (int i = 0; i < nt_; i++) {
            s.fidx[i]  = t_[i].fidx.f;
            s.ftag0[i] = t_[i].ftag[0].f;
            s.ftag1[i] = t_[i].ftag[1].f;
        }
        return s;
    }
    void restore(const Snapshot& s) {
        hist_ = s.h;
        for (int i = 0; i < nt_; i++) {
            t_[i].fidx.f    = s.fidx[i];
            t_[i].ftag[0].f = s.ftag0[i];
            t_[i].ftag[1].f = s.ftag1[i];
        }
    }

private:
    struct T {
        TageTableCfg          cfg;
        int                   idx_bits = 10;
        int                   ctr_max = 3, ctr_min = -4, u_max = 3;
        std::vector<int8_t>   ctr;
        std::vector<uint16_t> tag;
        std::vector<uint8_t>  u;
        Folded                fidx;
        Folded                ftag[2];
    };

    TageCfg              cfg_;
    int                  nt_       = 0;
    int                  bim_bits_ = 13;
    int                  bim_max_  = 3;
    int                  bim_thr_  = 2;
    std::vector<uint8_t> bim_;
    std::vector<T>       t_;
    HistReg              hist_;
    int                  use_alt_ctr_ = 8;
    long                 tick_        = 0;
    mutable uint64_t     rng_         = 1;

    uint64_t rnd() const {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;
        return rng_;
    }
    uint32_t bim_index(uint64_t pc) const {
        return (uint32_t)((pc >> 1) & (((uint64_t)1 << bim_bits_) - 1));
    }
    void bim_update(uint32_t i, bool taken) {
        if (taken) { if (bim_[i] < bim_max_) bim_[i]++; }
        else       { if (bim_[i] > 0)        bim_[i]--; }
    }
    static void ctr_update(int8_t& c, bool taken, int lo, int hi) {
        if (taken) { if (c < hi) c++; }
        else       { if (c > lo) c--; }
    }
    uint32_t index(int i, uint64_t pc) const {
        const T& t = t_[i];
        uint64_t p = pc >> 1;
        int sh = t.idx_bits - i;
        if (sh < 1) sh = 1;
        uint32_t h = (uint32_t)(p ^ (p >> sh)) ^ t.fidx.f;
        return h & (uint32_t)(t.cfg.entries - 1);
    }
    uint32_t tag(int i, uint64_t pc) const {
        const T& t = t_[i];
        uint64_t p = pc >> 1;
        uint32_t h = (uint32_t)p ^ t.ftag[0].f ^ (t.ftag[1].f << 1);
        return h & ((1u << t.cfg.tag_bits) - 1u);
    }
};

/* ============================================================
 * FTB 風格的 fetch-block entry
 *
 * 真實 trace 的 fetch block 可能含 0、1 或多個分支（37.7% 的 CoreMark
 * fetch block 完全沒有分支），且 block 的結束位址在 RVC 下無法用
 * 「PC + 固定 byte 數」推導。所以 entry 必須同時記
 *   (a) block 裡的前 N 個分支（PC / 型別 / 目標）
 *   (b) block 的 fall-through 位址（沒有任何分支 taken 時的結束位址）
 * 這正是 XiangShan FTB / BOOM 的做法。
 * ============================================================ */
static const int FE_MAX_SLOTS = 4;

struct BrSlot {
    uint64_t pc           = 0;
    uint64_t target       = 0;
    uint8_t  type         = BR_NONE;
    bool     valid        = false;
    bool     target_valid = false;
};

/* 一次執行觀察到的 block（ground truth） */
struct BlkObs {
    int      n_br          = 0;
    BrSlot   br[FE_MAX_SLOTS];
    int      taken_idx     = -1;    /* 哪個 slot 是 taken，-1 = 沒有 */
    bool     complete      = false; /* 這次跑完整個 block（沒有 taken）→ 看到了全部分支 */
    uint64_t fallthru      = 0;
    bool     fallthru_valid = false;
};

struct FtbEntry {
    bool     valid          = false;
    uint64_t tag            = 0;
    BrSlot   br[FE_MAX_SLOTS];
    uint8_t  hint[FE_MAX_SLOTS] = {2, 2, 2, 2};   /* 每個 slot 一個 2-bit 方向 hint */
    uint64_t fallthru       = 0;
    bool     fallthru_valid = false;
    uint64_t lru            = 0;
};

/* 用一次觀察去更新 entry。只會覆寫這次真的看到的部分。 */
static inline void ftb_apply(FtbEntry& x, const BlkObs& o, int nslots) {
    if (nslots > FE_MAX_SLOTS) nslots = FE_MAX_SLOTS;
    int seen = o.n_br < nslots ? o.n_br : nslots;
    for (int i = 0; i < seen; i++) {
        bool     newslot = (!x.br[i].valid) || (x.br[i].pc != o.br[i].pc);
        uint64_t oldtgt  = x.br[i].target;
        bool     oldtv   = x.br[i].target_valid;
        x.br[i] = o.br[i];
        x.br[i].valid = true;
        if (!o.br[i].target_valid && !newslot) {      /* 目標未知就沿用舊的 */
            x.br[i].target       = oldtgt;
            x.br[i].target_valid = oldtv;
        }
        if (newslot) x.hint[i] = (o.taken_idx == i) ? 2 : 1;
    }
    if (o.complete)                      /* 看到整個 block，後面確定沒有分支 */
        for (int i = seen; i < nslots; i++) x.br[i] = BrSlot();
    if (o.fallthru_valid) { x.fallthru = o.fallthru; x.fallthru_valid = true; }

    /* hint 只更新這次真的執行到的 slot */
    int last = (o.taken_idx >= 0) ? o.taken_idx : seen - 1;
    for (int i = 0; i <= last && i < nslots; i++) {
        if (br_is_uncond(x.br[i].type))      x.hint[i] = 3;
        else if (o.taken_idx == i) { if (x.hint[i] < 3) x.hint[i]++; }
        else                       { if (x.hint[i] > 0) x.hint[i]--; }
    }
    x.valid = true;
}

/* ============================================================
 * uBTB：全關聯，tag = fetch block PC 低位 hash，LRU，0 bubble
 * ============================================================ */
class UBtb {
public:
    void init(int entries, int tag_bits, int slots) {
        n_        = entries > 0 ? entries : 1;
        tag_bits_ = clampi(tag_bits, 8, 32);
        slots_    = clampi(slots, 1, FE_MAX_SLOTS);
        e_.assign((size_t)n_, FtbEntry());
        clk_ = 0;
    }
    int slots() const { return slots_; }
    uint32_t hash(uint64_t pc) const {
        uint64_t p = pc >> 1;
        uint64_t h = p ^ (p >> 13) ^ (p >> 27);
        return (uint32_t)(h & ((1ull << tag_bits_) - 1));
    }
    const FtbEntry* lookup(uint64_t pc) {
        uint32_t t = hash(pc);
        for (int i = 0; i < n_; i++)
            if (e_[i].valid && e_[i].tag == t) { e_[i].lru = ++clk_; return &e_[i]; }
        return nullptr;
    }
    void update(uint64_t pc, const BlkObs& o) {
        uint32_t t = hash(pc);
        int      v = -1;
        for (int i = 0; i < n_; i++)
            if (e_[i].valid && e_[i].tag == t) { v = i; break; }
        if (v < 0) {
            v = 0;
            for (int i = 0; i < n_; i++) {
                if (!e_[i].valid) { v = i; break; }
                if (e_[i].lru < e_[v].lru) v = i;
            }
            e_[v] = FtbEntry();
            e_[v].tag = t;
        }
        ftb_apply(e_[v], o, slots_);
        e_[v].lru = ++clk_;
    }

private:
    int                   n_ = 16, tag_bits_ = 20, slots_ = 1;
    std::vector<FtbEntry> e_;
    uint64_t              clk_ = 0;
};

/* ============================================================
 * L2 BTB：set associative + LRU，override uBTB 時 1 bubble
 * ============================================================ */
class Btb {
public:
    void init(int entries, int ways, int slots) {
        ways_ = ways > 0 ? ways : 1;
        int sets = entries / ways_;
        if (sets < 1) sets = 1;
        sets_     = 1 << log2ceil((uint64_t)sets);
        idx_bits_ = log2ceil((uint64_t)sets_);
        slots_    = clampi(slots, 1, FE_MAX_SLOTS);
        e_.assign((size_t)sets_ * ways_, FtbEntry());
        clk_ = 0;
    }
    int sets()  const { return sets_; }
    int ways()  const { return ways_; }
    int slots() const { return slots_; }

    const FtbEntry* lookup(uint64_t pc) {
        uint32_t s = index(pc);
        uint64_t t = tag(pc);
        for (int w = 0; w < ways_; w++) {
            FtbEntry& x = e_[(size_t)s * ways_ + w];
            if (x.valid && x.tag == t) { x.lru = ++clk_; return &x; }
        }
        return nullptr;
    }
    void update(uint64_t pc, const BlkObs& o) {
        uint32_t s = index(pc);
        uint64_t t = tag(pc);
        int      v = -1;
        for (int w = 0; w < ways_; w++)
            if (e_[(size_t)s * ways_ + w].valid && e_[(size_t)s * ways_ + w].tag == t) { v = w; break; }
        if (v < 0) {
            v = 0;
            for (int w = 0; w < ways_; w++) {
                FtbEntry& x = e_[(size_t)s * ways_ + w];
                if (!x.valid) { v = w; break; }
                if (x.lru < e_[(size_t)s * ways_ + v].lru) v = w;
            }
            e_[(size_t)s * ways_ + v] = FtbEntry();
            e_[(size_t)s * ways_ + v].tag = t;
        }
        FtbEntry& x = e_[(size_t)s * ways_ + v];
        ftb_apply(x, o, slots_);
        x.lru = ++clk_;
    }

private:
    int                   sets_ = 2048, ways_ = 4, idx_bits_ = 11, slots_ = 2;
    std::vector<FtbEntry> e_;
    uint64_t              clk_ = 0;

    uint32_t index(uint64_t pc) const { return (uint32_t)((pc >> 1) & (sets_ - 1)); }
    uint64_t tag(uint64_t pc) const   { return (pc >> 1) >> idx_bits_; }
};

/* ============================================================
 * RAS：call push / ret pop，環狀覆寫
 * ============================================================ */
class Ras {
public:
    struct Ent {
        uint64_t addr    = 0;   /* 推測的回傳位址        */
        uint64_t call_pc = 0;   /* 推它的 call 指令 PC    */
        bool     valid   = false;
    };

    void init(int depth) {
        n_ = depth > 0 ? depth : 1;
        s_.assign((size_t)n_, Ent());
        top_ = 0;
        cnt_ = 0;
    }
    uint64_t peek() const { return cnt_ ? s_[(size_t)((top_ + n_ - 1) % n_)].addr : 0; }
    void push(uint64_t a, uint64_t call_pc) {
        Ent e;
        e.addr    = a;
        e.call_pc = call_pc;
        e.valid   = true;
        s_[(size_t)top_] = e;
        top_ = (top_ + 1) % n_;
        if (cnt_ < n_) cnt_++;
    }
    Ent pop() {
        if (!cnt_) return Ent();
        top_ = (top_ + n_ - 1) % n_;
        cnt_--;
        return s_[(size_t)top_];
    }
    struct Snapshot { std::vector<Ent> s; int top, cnt; };
    Snapshot save() const { return Snapshot{s_, top_, cnt_}; }
    void restore(const Snapshot& x) { s_ = x.s; top_ = x.top; cnt_ = x.cnt; }

private:
    int              n_ = 16, top_ = 0, cnt_ = 0;
    std::vector<Ent> s_;
};

} /* namespace fe */
#endif /* BSTM_PREDICTORS_HPP */
