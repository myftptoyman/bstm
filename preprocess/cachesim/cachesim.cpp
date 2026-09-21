/* ============================================================================
 * BSTM — offline trace-driven cache simulator   (Agent B, preprocess/cachesim)
 *
 * Reads a memory access stream, simulates L1I / L1D / L2 (/ optional L3) plus
 * ITLB / DTLB and a stride prefetcher, and emits one overlay byte per access
 * in the `.mem` format defined by include/bstf.h.
 *
 * IMPORTANT — modelling scope (see doc/PLAN.md §7.4):
 *   The latency reported here is the *unloaded* latency.  There is deliberately
 *   NO MSHR model, NO bank conflict model, NO bus/queue model.  Queueing and
 *   memory level parallelism belong to the timing model, not to this tool.
 *
 * Build:   make
 * Usage:   cachesim <mem-stream> <config.json> <out.mem> [options]
 * ==========================================================================*/
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>

#include "bstf.h"   /* read-only contract header (include/bstf.h) */

/* ===========================================================================
 * 1. Minimal JSON parser (objects / arrays / numbers / strings / bool / null)
 *    Extension: `//` and "slash-star" comments are skipped, so config files
 *    can be annotated.
 * =========================================================================*/
namespace mj {

struct Val;
using Obj = std::vector<std::pair<std::string, Val> >;

struct Val {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t;
    bool        b;
    double      num;
    std::string str;
    std::vector<Val> arr;
    Obj         obj;
    Val() : t(NUL), b(false), num(0) {}

    const Val* find(const char* k) const {
        if (t != OBJ) return 0;
        for (size_t i = 0; i < obj.size(); i++)
            if (obj[i].first == k) return &obj[i].second;
        return 0;
    }
    bool has(const char* k) const { return find(k) != 0; }
    double get_num(const char* k, double d) const {
        const Val* v = find(k); return (v && v->t == NUM) ? v->num : d;
    }
    bool get_bool(const char* k, bool d) const {
        const Val* v = find(k);
        if (!v) return d;
        if (v->t == BOOL) return v->b;
        if (v->t == NUM)  return v->num != 0;
        return d;
    }
    std::string get_str(const char* k, const char* d) const {
        const Val* v = find(k); return (v && v->t == STR) ? v->str : std::string(d);
    }
    const Val* get_obj(const char* k) const {
        const Val* v = find(k); return (v && v->t == OBJ) ? v : 0;
    }
};

struct Parser {
    const char* p;
    const char* end;
    explicit Parser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    [[noreturn]] void fail(const char* msg) {
        throw std::runtime_error(std::string("config JSON: ") + msg);
    }
    void ws() {
        for (;;) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (p + 1 < end && p[0] == '/' && p[1] == '/') {
                while (p < end && *p != '\n') p++;
                continue;
            }
            if (p + 1 < end && p[0] == '/' && p[1] == '*') {
                p += 2;
                while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
                p = (p + 1 < end) ? p + 2 : end;
                continue;
            }
            return;
        }
    }
    Val parse() { ws(); Val v = value(); ws(); return v; }

    Val value() {
        ws();
        if (p >= end) fail("unexpected end of input");
        char c = *p;
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') { Val v; v.t = Val::STR; v.str = string(); return v; }
        if (c == 't') { expect("true");  Val v; v.t = Val::BOOL; v.b = true;  return v; }
        if (c == 'f') { expect("false"); Val v; v.t = Val::BOOL; v.b = false; return v; }
        if (c == 'n') { expect("null");  return Val(); }
        return number();
    }
    void expect(const char* lit) {
        size_t n = strlen(lit);
        if ((size_t)(end - p) < n || strncmp(p, lit, n) != 0) fail("bad literal");
        p += n;
    }
    Val number() {
        char* e = 0;
        double d = strtod(p, &e);
        if (e == p) fail("bad number");
        p = e;
        Val v; v.t = Val::NUM; v.num = d; return v;
    }
    std::string string() {
        if (*p != '"') fail("expected string");
        p++;
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                switch (*p) {
                    case 'n': s += '\n'; break; case 't': s += '\t'; break;
                    case 'r': s += '\r'; break; case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    default:  s += *p;   break;
                }
                p++;
            } else s += *p++;
        }
        if (p >= end) fail("unterminated string");
        p++;
        return s;
    }
    Val array() {
        Val v; v.t = Val::ARR; p++; ws();
        if (p < end && *p == ']') { p++; return v; }
        for (;;) {
            v.arr.push_back(value());
            ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == ']') { p++; return v; }
            fail("expected , or ] in array");
        }
    }
    Val object() {
        Val v; v.t = Val::OBJ; p++; ws();
        if (p < end && *p == '}') { p++; return v; }
        for (;;) {
            ws();
            std::string k = string();
            ws();
            if (p >= end || *p != ':') fail("expected : in object");
            p++;
            v.obj.push_back(std::make_pair(k, value()));
            ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { p++; return v; }
            fail("expected , or } in object");
        }
    }
};

} /* namespace mj */

/* ===========================================================================
 * 2. Set-associative LRU cache
 * =========================================================================*/
enum { FL_DIRTY = 1, FL_PREFETCH = 2 };

struct Victim {
    bool     valid;
    uint64_t line;
    bool     dirty;
    bool     prefetch;   /* evicted without ever being used by a demand access */
    Victim() : valid(false), line(0), dirty(false), prefetch(false) {}
};

class Cache {
public:
    uint32_t sets, ways, set_mask;
    std::vector<uint64_t> tag;   /* full line address, MRU first inside a set */
    std::vector<uint8_t>  fl;
    std::vector<uint8_t>  cnt;

    Cache() : sets(0), ways(0), set_mask(0) {}

    void init(uint32_t s, uint32_t w) {
        sets = s; ways = w; set_mask = s - 1;
        tag.assign((size_t)s * w, 0);
        fl.assign((size_t)s * w, 0);
        cnt.assign(s, 0);
    }
    bool enabled() const { return sets != 0; }

    inline uint32_t setof(uint64_t la) const { return (uint32_t)(la & set_mask); }

    inline int find(uint32_t s, uint64_t la) const {
        size_t base = (size_t)s * ways;
        for (uint32_t i = 0; i < cnt[s]; i++)
            if (tag[base + i] == la) return (int)i;
        return -1;
    }
    inline void mtf(uint32_t s, int i) {
        if (i == 0) return;
        size_t base = (size_t)s * ways;
        uint64_t t = tag[base + i];
        uint8_t  f = fl[base + i];
        for (int j = i; j > 0; j--) { tag[base + j] = tag[base + j - 1]; fl[base + j] = fl[base + j - 1]; }
        tag[base] = t; fl[base] = f;
    }

    /* Returns true on hit.  `v` receives the evicted line (if any) on a miss. */
    bool access(uint64_t la, bool write, bool as_prefetch, bool demand,
                Victim& v, bool* pf_useful)
    {
        v = Victim();
        uint32_t s = setof(la);
        size_t base = (size_t)s * ways;
        int i = find(s, la);
        if (i >= 0) {
            if (demand && (fl[base + i] & FL_PREFETCH)) {
                if (pf_useful) *pf_useful = true;
                fl[base + i] &= (uint8_t)~FL_PREFETCH;
            }
            if (write) fl[base + i] |= FL_DIRTY;
            mtf(s, i);
            return true;
        }
        /* miss -> allocate at MRU */
        if (cnt[s] == ways) {
            v.valid    = true;
            v.line     = tag[base + ways - 1];
            v.dirty    = (fl[base + ways - 1] & FL_DIRTY) != 0;
            v.prefetch = (fl[base + ways - 1] & FL_PREFETCH) != 0;
        } else {
            cnt[s]++;
        }
        for (int j = (int)cnt[s] - 1; j > 0; j--) {
            tag[base + j] = tag[base + j - 1];
            fl[base + j]  = fl[base + j - 1];
        }
        tag[base] = la;
        fl[base]  = (uint8_t)((write ? FL_DIRTY : 0) | (as_prefetch ? FL_PREFETCH : 0));
        return false;
    }

    bool probe(uint64_t la) const { return find(setof(la), la) >= 0; }

    /* remove a line; returns true if present, *was_dirty set accordingly */
    bool invalidate(uint64_t la, bool* was_dirty) {
        uint32_t s = setof(la);
        size_t base = (size_t)s * ways;
        int i = find(s, la);
        if (i < 0) return false;
        if (was_dirty) *was_dirty = (fl[base + i] & FL_DIRTY) != 0;
        for (uint32_t j = (uint32_t)i; j + 1 < cnt[s]; j++) {
            tag[base + j] = tag[base + j + 1];
            fl[base + j]  = fl[base + j + 1];
        }
        cnt[s]--;
        return true;
    }
};

/* ===========================================================================
 * 3. Stride prefetcher (trains on demand accesses, fills into L2)
 *
 * The access stream carries no PC by default, so the training table is indexed
 * either by PC (if the optional 5th stream column is present) or by the page
 * number of the address ("page" mode) — see README.
 * =========================================================================*/
struct StrideEnt {
    uint64_t key;
    uint64_t last;
    int64_t  stride;
    uint8_t  conf;
    bool     valid;
    StrideEnt() : key(0), last(0), stride(0), conf(0), valid(false) {}
};

class StridePF {
public:
    std::vector<StrideEnt> tbl;
    uint32_t entries;
    uint32_t degree;      /* how many lines to fetch ahead                */
    uint32_t distance;    /* first prefetch is addr + stride*distance     */
    uint8_t  min_conf;
    bool     index_pc;

    StridePF() : entries(0), degree(2), distance(1), min_conf(2), index_pc(true) {}

    void init(uint32_t n, uint32_t deg, uint32_t dist, uint32_t mc, bool by_pc) {
        entries = n; degree = deg; distance = dist;
        min_conf = (uint8_t)mc; index_pc = by_pc;
        tbl.assign(n, StrideEnt());
    }
    static uint64_t mix(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
    /* returns prefetch target *addresses* (caller converts to lines) */
    void update(uint64_t key, uint64_t addr, std::vector<uint64_t>& out) {
        out.clear();
        if (!entries) return;
        uint32_t i = (uint32_t)(mix(key) % entries);
        StrideEnt& e = tbl[i];
        if (!e.valid || e.key != key) {
            e.valid = true; e.key = key; e.last = addr; e.stride = 0; e.conf = 0;
            return;
        }
        int64_t d = (int64_t)addr - (int64_t)e.last;
        if (d == 0) return;                 /* same address again: no training */
        if (d == e.stride) { if (e.conf < 3) e.conf++; }
        else               { e.stride = d; e.conf = 0; }
        e.last = addr;
        if (e.conf >= min_conf && e.stride != 0) {
            for (uint32_t k = 0; k < degree; k++) {
                int64_t off = e.stride * (int64_t)(distance + k);
                out.push_back(addr + (uint64_t)off);
            }
        }
    }
};

/* ===========================================================================
 * 4. Multi-configuration single-scan engine (LRU stack distance)
 *
 * All variants share line size.  The "fine" partition is the set partition of
 * the variant with the most sets; because every variant's set index is the low
 * log2(S_c) bits of the line address, the fine partition refines every
 * variant's partition, so:
 *
 *   rank_fine(L) <= rank_group_c(L)      (a fine set is a subset of a group)
 *
 * A line is a hit in variant c iff the number of *distinct* lines of its group
 * touched more recently than it is < ways_c.  Consequently a line whose fine
 * rank already exceeds max_ways can never hit in any variant, so each fine list
 * can be truncated to max_ways entries — total state = size of largest variant.
 * =========================================================================*/
class StackSim {
public:
    struct E { uint64_t la; uint64_t ts; };
    std::vector<std::vector<E> > fine;
    std::vector<uint32_t> c_sets, c_ways;
    uint32_t fine_sets, max_ways;
    uint64_t clk;

    StackSim() : fine_sets(0), max_ways(0), clk(0) {}

    void init(const std::vector<uint32_t>& sets, const std::vector<uint32_t>& ways) {
        c_sets = sets; c_ways = ways;
        fine_sets = 1; max_ways = 1;
        for (size_t i = 0; i < sets.size(); i++) {
            if (sets[i] > fine_sets) fine_sets = sets[i];
            if (ways[i] > max_ways)  max_ways  = ways[i];
        }
        fine.assign(fine_sets, std::vector<E>());
    }
    size_t nconfigs() const { return c_sets.size(); }

    /* demand=false -> state is updated but no hit/miss is reported */
    void access(uint64_t la, bool demand, std::vector<uint64_t>& hits,
                std::vector<uint64_t>& accs)
    {
        uint32_t f = (uint32_t)(la & (fine_sets - 1));
        std::vector<E>& L = fine[f];
        int p = -1;
        for (size_t i = 0; i < L.size(); i++) if (L[i].la == la) { p = (int)i; break; }

        if (p < 0) {
            if (demand) for (size_t c = 0; c < c_sets.size(); c++) accs[c]++;
            E e; e.la = la; e.ts = ++clk;
            L.insert(L.begin(), e);
            if (L.size() > max_ways) L.resize(max_ways);
            return;
        }
        uint64_t t = L[p].ts;
        if (demand) {
            for (size_t c = 0; c < c_sets.size(); c++) {
                uint32_t S = c_sets[c], W = c_ways[c];
                uint32_t count = (uint32_t)p;
                uint32_t i0 = (uint32_t)(la & (S - 1));
                for (uint32_t g = i0; g < fine_sets && count < W; g += S) {
                    if (g == f) continue;
                    const std::vector<E>& G = fine[g];
                    for (size_t j = 0; j < G.size(); j++) {
                        if (G[j].ts <= t) break;      /* lists are MRU-first */
                        if (++count >= W) break;
                    }
                }
                accs[c]++;
                if (count < W) hits[c]++;
            }
        }
        E e = L[(size_t)p];
        e.ts = ++clk;
        L.erase(L.begin() + p);
        L.insert(L.begin(), e);
    }
};

/* ===========================================================================
 * 5. Configuration
 * =========================================================================*/
struct LvlCfg {
    bool     enabled;
    uint64_t size;
    uint32_t ways;
    uint32_t sets;
    LvlCfg() : enabled(false), size(0), ways(0), sets(0) {}
};

struct Config {
    uint32_t line_size, line_bits;
    uint32_t page_size, page_bits;
    LvlCfg   l1i, l1d, l2, l3;
    bool     l2_inclusive;
    uint32_t dtlb_entries, itlb_entries;
    bool     pf_enabled;
    uint32_t pf_entries, pf_degree, pf_distance, pf_minconf;
    bool     pf_index_pc;
    /* latency classes (indices into bstf_lat_table[]) */
    uint32_t lc_l1, lc_l2, lc_l3, lc_dram, lc_pf, lc_store_hit;
    bool     tlb_miss_bump;
    /* sweep */
    std::string sweep_level;
    std::vector<uint64_t> sweep_sizes;
    uint32_t sweep_ways;
    uint64_t icount;

    Config() : line_size(64), line_bits(6), page_size(4096), page_bits(12),
               l2_inclusive(true), dtlb_entries(64), itlb_entries(32),
               pf_enabled(true), pf_entries(64), pf_degree(2), pf_distance(1),
               pf_minconf(2), pf_index_pc(true),
               lc_l1(0), lc_l2(2), lc_l3(4), lc_dram(6), lc_pf(11), lc_store_hit(0),
               tlb_miss_bump(false), sweep_ways(0), icount(0) {}
};

static uint32_t log2u(uint64_t v) { uint32_t r = 0; while ((1ULL << r) < v) r++; return r; }
static bool is_pow2(uint64_t v)   { return v && ((v & (v - 1)) == 0); }

static void cfg_level(const mj::Val& root, const char* name, LvlCfg& L,
                      uint64_t defsize, uint32_t defways, bool defen, uint32_t line)
{
    const mj::Val* o = root.get_obj(name);
    L.enabled = defen;
    L.size = defsize; L.ways = defways;
    if (o) {
        L.enabled = o->get_bool("enabled", true);
        L.size    = (uint64_t)o->get_num("size",  (double)defsize);
        L.ways    = (uint32_t)o->get_num("ways",  (double)defways);
    }
    if (!L.enabled) { L.sets = 0; return; }
    if (!L.ways || !L.size) throw std::runtime_error(std::string(name) + ": bad size/ways");
    uint64_t s = L.size / ((uint64_t)L.ways * line);
    if (!s || !is_pow2(s))
        throw std::runtime_error(std::string(name) + ": size/(ways*line) must be a power of two");
    L.sets = (uint32_t)s;
}

static Config load_config(const char* path)
{
    std::string txt;
    if (path && strcmp(path, "-") != 0) {
        FILE* f = fopen(path, "rb");
        if (!f) throw std::runtime_error(std::string("cannot open config: ") + path);
        char buf[8192]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) txt.append(buf, n);
        fclose(f);
    } else txt = "{}";

    mj::Parser P(txt);
    mj::Val root = P.parse();
    if (root.t != mj::Val::OBJ) throw std::runtime_error("config root must be an object");

    Config c;
    c.line_size = (uint32_t)root.get_num("line_size", 64);
    if (!is_pow2(c.line_size)) throw std::runtime_error("line_size must be a power of two");
    c.line_bits = log2u(c.line_size);
    c.page_size = (uint32_t)root.get_num("page_size", 4096);
    if (!is_pow2(c.page_size)) throw std::runtime_error("page_size must be a power of two");
    c.page_bits = log2u(c.page_size);

    cfg_level(root, "l1i", c.l1i,   32u * 1024,  8, true,  c.line_size);
    cfg_level(root, "l1d", c.l1d,   32u * 1024,  8, true,  c.line_size);
    cfg_level(root, "l2",  c.l2,   512u * 1024,  8, true,  c.line_size);
    cfg_level(root, "l3",  c.l3,  4096u * 1024, 16, true,  c.line_size);

    const mj::Val* o = root.get_obj("l2");
    c.l2_inclusive = o ? o->get_bool("inclusive", true) : true;

    o = root.get_obj("dtlb"); c.dtlb_entries = o ? (uint32_t)o->get_num("entries", 64) : 64;
    o = root.get_obj("itlb"); c.itlb_entries = o ? (uint32_t)o->get_num("entries", 32) : 32;

    o = root.get_obj("prefetcher");
    if (o) {
        c.pf_enabled  = o->get_bool("enabled", true);
        c.pf_entries  = (uint32_t)o->get_num("entries", 64);
        c.pf_degree   = (uint32_t)o->get_num("degree", 2);
        c.pf_distance = (uint32_t)o->get_num("distance", 1);
        c.pf_minconf  = (uint32_t)o->get_num("min_conf", 2);
        c.pf_index_pc = (o->get_str("index", "pc") == "pc");
    }
    o = root.get_obj("latency_class");
    if (o) {
        c.lc_l1        = (uint32_t)o->get_num("l1",   c.lc_l1);
        c.lc_l2        = (uint32_t)o->get_num("l2",   c.lc_l2);
        c.lc_l3        = (uint32_t)o->get_num("l3",   c.lc_l3);
        c.lc_dram      = (uint32_t)o->get_num("dram", c.lc_dram);
        c.lc_pf        = (uint32_t)o->get_num("prefetch",   c.lc_pf);
        c.lc_store_hit = (uint32_t)o->get_num("store_hit",  c.lc_store_hit);
    }
    c.tlb_miss_bump = root.get_bool("tlb_miss_bump_class", false);
    c.icount = (uint64_t)root.get_num("icount", 0);

    o = root.get_obj("sweep");
    if (o) {
        c.sweep_level = o->get_str("level", "");
        c.sweep_ways  = (uint32_t)o->get_num("ways", 0);
        const mj::Val* a = o->find("sizes");
        if (a && a->t == mj::Val::ARR)
            for (size_t i = 0; i < a->arr.size(); i++)
                c.sweep_sizes.push_back((uint64_t)a->arr[i].num);
        if (!c.sweep_sizes.empty() &&
            c.sweep_level != "l1i" && c.sweep_level != "l1d" && c.sweep_level != "l2")
            throw std::runtime_error("sweep.level must be one of l1i / l1d / l2");
    }
    return c;
}

/* ===========================================================================
 * 6. Statistics
 * =========================================================================*/
struct Stats {
    uint64_t n_access, n_load, n_store, n_ifetch, n_split;
    uint64_t l1i_acc, l1i_hit, l1d_acc, l1d_hit;
    uint64_t l2_acc,  l2_hit,  l3_acc,  l3_hit, dram_acc;
    uint64_t dtlb_acc, dtlb_miss, itlb_acc, itlb_miss;
    uint64_t pf_issued, pf_redundant, pf_useful, pf_evicted_unused;
    uint64_t l1d_wb, l2_wb, dram_wb, back_inval;
    uint64_t lvl_cnt[4];
    uint64_t class_cnt[16];
};

/* ===========================================================================
 * 7. Simulator
 * =========================================================================*/
struct LineRes { int level; bool pf_hit; };

class Sim {
public:
    Config  cfg;
    Cache   l1i, l1d, l2, l3, itlb, dtlb;
    StridePF pf;
    Stats   st{};
    std::vector<uint64_t> pf_tgt;

    /* sweep state */
    int      sweep_on;          /* 0 none, 1 l1i, 2 l1d, 3 l2 */
    StackSim sweep;
    std::vector<uint64_t> sw_hit, sw_acc;

    explicit Sim(const Config& c) : cfg(c), sweep_on(0) {
        if (cfg.l1i.enabled) l1i.init(cfg.l1i.sets, cfg.l1i.ways);
        if (cfg.l1d.enabled) l1d.init(cfg.l1d.sets, cfg.l1d.ways);
        if (cfg.l2.enabled)  l2.init(cfg.l2.sets,  cfg.l2.ways);
        if (cfg.l3.enabled)  l3.init(cfg.l3.sets,  cfg.l3.ways);
        if (cfg.itlb_entries) itlb.init(1, cfg.itlb_entries);
        if (cfg.dtlb_entries) dtlb.init(1, cfg.dtlb_entries);
        if (cfg.pf_enabled)
            pf.init(cfg.pf_entries, cfg.pf_degree, cfg.pf_distance,
                    cfg.pf_minconf, cfg.pf_index_pc);

        if (!cfg.sweep_sizes.empty()) {
            uint32_t base_ways = cfg.sweep_ways;
            if (!base_ways) {
                if (cfg.sweep_level == "l1i") base_ways = cfg.l1i.ways;
                else if (cfg.sweep_level == "l1d") base_ways = cfg.l1d.ways;
                else base_ways = cfg.l2.ways;
            }
            std::vector<uint32_t> sets, ways;
            for (size_t i = 0; i < cfg.sweep_sizes.size(); i++) {
                uint64_t s = cfg.sweep_sizes[i] / ((uint64_t)base_ways * cfg.line_size);
                if (!s || !is_pow2(s))
                    throw std::runtime_error("sweep: size/(ways*line) must be a power of two");
                sets.push_back((uint32_t)s);
                ways.push_back(base_ways);
            }
            sweep.init(sets, ways);
            sw_hit.assign(sets.size(), 0);
            sw_acc.assign(sets.size(), 0);
            sweep_on = (cfg.sweep_level == "l1i") ? 1 :
                       (cfg.sweep_level == "l1d") ? 2 : 3;
        }
    }

    inline uint64_t lineof(uint64_t a) const { return a >> cfg.line_bits; }

    void l3_evict(const Victim& v) { if (v.valid && v.dirty) st.dram_wb++; }

    void l2_evict(const Victim& v) {
        if (!v.valid) return;
        bool dirty = v.dirty;
        if (cfg.l2_inclusive) {
            bool d = false;
            if (l1d.enabled() && l1d.invalidate(v.line, &d)) { st.back_inval++; if (d) dirty = true; }
            if (l1i.enabled() && l1i.invalidate(v.line, 0))    st.back_inval++;
        }
        if (v.prefetch) st.pf_evicted_unused++;
        if (dirty) {
            st.l2_wb++;
            if (l3.enabled()) { Victim v3; l3.access(v.line, true, false, false, v3, 0); l3_evict(v3); }
            else st.dram_wb++;
        }
    }

    inline void sweep_l2(uint64_t la, bool demand) {
        if (sweep_on == 3) sweep.access(la, demand, sw_hit, sw_acc);
    }

    /* fill/allocate into L2 for a writeback or a prefetch */
    void l2_insert(uint64_t la, bool dirty, bool as_pf) {
        if (!l2.enabled()) { if (dirty) st.dram_wb++; return; }
        Victim v;
        l2.access(la, dirty, as_pf, false, v, 0);
        sweep_l2(la, false);
        l2_evict(v);
    }

    void issue_prefetch(uint64_t addr) {
        uint64_t la = lineof(addr);
        if ((l1d.enabled() && l1d.probe(la)) || (l2.enabled() && l2.probe(la))) {
            st.pf_redundant++;
            return;
        }
        st.pf_issued++;
        if (l3.enabled()) { Victim v3; l3.access(la, false, false, false, v3, 0); l3_evict(v3); }
        l2_insert(la, false, true);
    }

    LineRes access_line(uint64_t la, bool store, bool ifetch) {
        LineRes r; r.level = 3; r.pf_hit = false;
        Cache& l1 = ifetch ? l1i : l1d;
        bool write = store && !ifetch;

        if (sweep_on == 1 && ifetch)  sweep.access(la, true, sw_hit, sw_acc);
        if (sweep_on == 2 && !ifetch) sweep.access(la, true, sw_hit, sw_acc);

        Victim v1;
        bool h1 = false;
        if (l1.enabled()) {
            h1 = l1.access(la, write, false, true, v1, 0);
            if (ifetch) { st.l1i_acc++; if (h1) st.l1i_hit++; }
            else        { st.l1d_acc++; if (h1) st.l1d_hit++; }
        }
        if (h1) { r.level = 0; return r; }

        /* ---- L1 miss: go to L2 ---- */
        bool pfu = false;
        if (l2.enabled()) {
            Victim v2;
            bool h2 = l2.access(la, false, false, true, v2, &pfu);
            sweep_l2(la, true);
            st.l2_acc++;
            if (h2) st.l2_hit++;
            l2_evict(v2);
            if (pfu) st.pf_useful++;
            if (h2) { r.level = 1; r.pf_hit = pfu; goto wb; }
        }
        /* ---- L2 miss: L3 / DRAM ---- */
        if (l3.enabled()) {
            Victim v3;
            bool h3 = l3.access(la, false, false, true, v3, 0);
            st.l3_acc++;
            if (h3) st.l3_hit++;
            l3_evict(v3);
            r.level = h3 ? 2 : 3;
            if (!h3) st.dram_acc++;
        } else {
            r.level = 3;
            st.dram_acc++;
        }
    wb:
        /* ---- writeback of the L1D victim ---- */
        if (v1.valid && v1.dirty) { st.l1d_wb++; l2_insert(v1.line, true, false); }
        return r;
    }

    /* returns the overlay byte for one access */
    uint8_t access(uint64_t va, uint32_t size, bool store, bool ifetch,
                   uint64_t pckey, bool has_pc)
    {
        if (!size) size = 1;
        st.n_access++;
        if (ifetch)      st.n_ifetch++;
        else if (store)  st.n_store++;
        else             st.n_load++;

        /* ---- TLB (one lookup per distinct page touched) ---- */
        bool tlb_miss = false;
        Cache& tlb = ifetch ? itlb : dtlb;
        uint64_t vpn0 = va >> cfg.page_bits;
        uint64_t vpn1 = (va + size - 1) >> cfg.page_bits;
        if (tlb.enabled()) {
            for (uint64_t p = vpn0; p <= vpn1; p++) {
                Victim vt;
                bool h = tlb.access(p, false, false, true, vt, 0);
                if (ifetch) { st.itlb_acc++; if (!h) st.itlb_miss++; }
                else        { st.dtlb_acc++; if (!h) st.dtlb_miss++; }
                if (!h) tlb_miss = true;
            }
        }

        /* ---- cache lines touched ---- */
        uint64_t la0 = lineof(va), la1 = lineof(va + size - 1);
        if (la1 != la0) st.n_split++;
        LineRes best; best.level = -1; best.pf_hit = false;
        for (uint64_t la = la0; la <= la1; la++) {
            LineRes r = access_line(la, store, ifetch);
            if (r.level > best.level) best = r;
        }

        /* ---- stride prefetcher trains on data accesses only ---- */
        if (cfg.pf_enabled && !ifetch) {
            uint64_t key = (cfg.pf_index_pc && has_pc) ? pckey : (va >> cfg.page_bits);
            pf.update(key, va, pf_tgt);
            for (size_t i = 0; i < pf_tgt.size(); i++) issue_prefetch(pf_tgt[i]);
        }

        /* ---- encode ---- */
        uint32_t lc;
        switch (best.level) {
            case 0:  lc = store ? cfg.lc_store_hit : cfg.lc_l1; break;
            case 1:  lc = best.pf_hit ? cfg.lc_pf : cfg.lc_l2;  break;
            case 2:  lc = cfg.lc_l3;   break;
            default: lc = cfg.lc_dram; break;
        }
        if (tlb_miss && cfg.tlb_miss_bump && lc < 15) lc++;
        uint8_t b = (uint8_t)(best.level & 0x3);
        if (best.pf_hit) b |= MEM_PREFETCH_HIT;
        if (tlb_miss)    b |= MEM_TLB_MISS;
        b |= (uint8_t)((lc & 0xF) << 4);

        st.lvl_cnt[best.level & 3]++;
        st.class_cnt[lc & 0xF]++;
        return b;
    }
};

/* ===========================================================================
 * 8. Buffered line reader + field parsing
 * =========================================================================*/
class LineReader {
    FILE* f;
    std::vector<char> buf;
    size_t pos, len;
    bool   eof;
public:
    explicit LineReader(FILE* fp) : f(fp), buf(1 << 20), pos(0), len(0), eof(false) {}
    bool next(char*& b, char*& e) {
        for (;;) {
            for (size_t i = pos; i < len; i++) {
                if (buf[i] == '\n') {
                    b = &buf[pos]; e = &buf[i];
                    pos = i + 1;
                    return true;
                }
            }
            if (eof) {
                if (pos < len) { b = &buf[pos]; e = &buf[len]; pos = len; return true; }
                return false;
            }
            /* compact + refill */
            if (pos > 0) { memmove(&buf[0], &buf[pos], len - pos); len -= pos; pos = 0; }
            if (len == buf.size()) buf.resize(buf.size() * 2);
            size_t n = fread(&buf[len], 1, buf.size() - len, f);
            if (n == 0) eof = true;
            len += n;
        }
    }
};

static inline uint64_t parse_hex(const char*& p, const char* e, bool& ok) {
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    if (p + 1 < e && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint64_t v = 0; int n = 0;
    while (p < e) {
        char c = *p;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint64_t)d; p++; n++;
    }
    ok = (n > 0);
    return v;
}
static inline uint64_t parse_dec(const char*& p, const char* e, bool& ok) {
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    uint64_t v = 0; int n = 0;
    while (p < e && *p >= '0' && *p <= '9') { v = v * 10 + (uint64_t)(*p - '0'); p++; n++; }
    ok = (n > 0);
    return v;
}

/* ===========================================================================
 * 9. Stats output
 * =========================================================================*/
static double ratio(uint64_t a, uint64_t b) { return b ? (double)a / (double)b : 0.0; }

static void write_stats(const char* path, const Sim& s, const Config& c, uint64_t icount)
{
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cachesim: cannot write %s\n", path); return; }
    const Stats& t = s.st;
    uint64_t ic = icount ? icount : (t.n_ifetch ? t.n_ifetch : t.n_access);
    const char* basis = icount ? "explicit" : (t.n_ifetch ? "ifetch_accesses" : "total_accesses");
    double kilo = (double)ic / 1000.0;

    uint64_t l1i_miss = t.l1i_acc - t.l1i_hit;
    uint64_t l1d_miss = t.l1d_acc - t.l1d_hit;
    uint64_t l2_miss  = t.l2_acc  - t.l2_hit;
    uint64_t l3_miss  = t.l3_acc  - t.l3_hit;

    fprintf(f, "{\n");
    fprintf(f, "  \"tool\": \"bstm-cachesim\",\n");
    fprintf(f, "  \"config\": {\n");
    fprintf(f, "    \"line_size\": %u, \"page_size\": %u,\n", c.line_size, c.page_size);
    fprintf(f, "    \"l1i\": {\"size\": %llu, \"ways\": %u, \"sets\": %u, \"enabled\": %s},\n",
            (unsigned long long)c.l1i.size, c.l1i.ways, c.l1i.sets, c.l1i.enabled ? "true" : "false");
    fprintf(f, "    \"l1d\": {\"size\": %llu, \"ways\": %u, \"sets\": %u, \"enabled\": %s},\n",
            (unsigned long long)c.l1d.size, c.l1d.ways, c.l1d.sets, c.l1d.enabled ? "true" : "false");
    fprintf(f, "    \"l2\": {\"size\": %llu, \"ways\": %u, \"sets\": %u, \"enabled\": %s, \"inclusive\": %s},\n",
            (unsigned long long)c.l2.size, c.l2.ways, c.l2.sets, c.l2.enabled ? "true" : "false",
            c.l2_inclusive ? "true" : "false");
    fprintf(f, "    \"l3\": {\"size\": %llu, \"ways\": %u, \"sets\": %u, \"enabled\": %s},\n",
            (unsigned long long)c.l3.size, c.l3.ways, c.l3.sets, c.l3.enabled ? "true" : "false");
    fprintf(f, "    \"dtlb_entries\": %u, \"itlb_entries\": %u,\n", c.dtlb_entries, c.itlb_entries);
    fprintf(f, "    \"prefetcher\": {\"enabled\": %s, \"entries\": %u, \"degree\": %u, "
               "\"distance\": %u, \"min_conf\": %u, \"index\": \"%s\"}\n",
            c.pf_enabled ? "true" : "false", c.pf_entries, c.pf_degree, c.pf_distance,
            c.pf_minconf, c.pf_index_pc ? "pc" : "page");
    fprintf(f, "  },\n");

    fprintf(f, "  \"accesses\": {\"total\": %llu, \"load\": %llu, \"store\": %llu, "
               "\"ifetch\": %llu, \"line_split\": %llu},\n",
            (unsigned long long)t.n_access, (unsigned long long)t.n_load,
            (unsigned long long)t.n_store, (unsigned long long)t.n_ifetch,
            (unsigned long long)t.n_split);
    fprintf(f, "  \"icount\": %llu, \"mpki_basis\": \"%s\",\n", (unsigned long long)ic, basis);

    fprintf(f, "  \"levels\": {\n");
    fprintf(f, "    \"l1i\": {\"accesses\": %llu, \"hits\": %llu, \"misses\": %llu, "
               "\"hit_rate\": %.6f, \"mpki\": %.4f},\n",
            (unsigned long long)t.l1i_acc, (unsigned long long)t.l1i_hit,
            (unsigned long long)l1i_miss, ratio(t.l1i_hit, t.l1i_acc), kilo ? l1i_miss / kilo : 0.0);
    fprintf(f, "    \"l1d\": {\"accesses\": %llu, \"hits\": %llu, \"misses\": %llu, "
               "\"hit_rate\": %.6f, \"mpki\": %.4f},\n",
            (unsigned long long)t.l1d_acc, (unsigned long long)t.l1d_hit,
            (unsigned long long)l1d_miss, ratio(t.l1d_hit, t.l1d_acc), kilo ? l1d_miss / kilo : 0.0);
    fprintf(f, "    \"l2\": {\"accesses\": %llu, \"hits\": %llu, \"misses\": %llu, "
               "\"hit_rate\": %.6f, \"mpki\": %.4f},\n",
            (unsigned long long)t.l2_acc, (unsigned long long)t.l2_hit,
            (unsigned long long)l2_miss, ratio(t.l2_hit, t.l2_acc), kilo ? l2_miss / kilo : 0.0);
    fprintf(f, "    \"l3\": {\"accesses\": %llu, \"hits\": %llu, \"misses\": %llu, "
               "\"hit_rate\": %.6f, \"mpki\": %.4f},\n",
            (unsigned long long)t.l3_acc, (unsigned long long)t.l3_hit,
            (unsigned long long)l3_miss, ratio(t.l3_hit, t.l3_acc), kilo ? l3_miss / kilo : 0.0);
    fprintf(f, "    \"dram\": {\"accesses\": %llu, \"mpki\": %.4f}\n",
            (unsigned long long)t.dram_acc, kilo ? t.dram_acc / kilo : 0.0);
    fprintf(f, "  },\n");

    fprintf(f, "  \"tlb\": {\n");
    fprintf(f, "    \"itlb\": {\"accesses\": %llu, \"misses\": %llu, \"miss_rate\": %.6f},\n",
            (unsigned long long)t.itlb_acc, (unsigned long long)t.itlb_miss,
            ratio(t.itlb_miss, t.itlb_acc));
    fprintf(f, "    \"dtlb\": {\"accesses\": %llu, \"misses\": %llu, \"miss_rate\": %.6f}\n",
            (unsigned long long)t.dtlb_acc, (unsigned long long)t.dtlb_miss,
            ratio(t.dtlb_miss, t.dtlb_acc));
    fprintf(f, "  },\n");

    uint64_t pf_demand_base = t.pf_useful + l2_miss;
    fprintf(f, "  \"prefetch\": {\"issued\": %llu, \"redundant\": %llu, \"useful\": %llu, "
               "\"evicted_unused\": %llu, \"accuracy\": %.6f, \"coverage\": %.6f},\n",
            (unsigned long long)t.pf_issued, (unsigned long long)t.pf_redundant,
            (unsigned long long)t.pf_useful, (unsigned long long)t.pf_evicted_unused,
            ratio(t.pf_useful, t.pf_issued), ratio(t.pf_useful, pf_demand_base));

    fprintf(f, "  \"writeback\": {\"l1d\": %llu, \"l2\": %llu, \"dram\": %llu, "
               "\"l2_back_invalidations\": %llu},\n",
            (unsigned long long)t.l1d_wb, (unsigned long long)t.l2_wb,
            (unsigned long long)t.dram_wb, (unsigned long long)t.back_inval);

    fprintf(f, "  \"overlay\": {\n    \"level_histogram\": {\"l1\": %llu, \"l2\": %llu, "
               "\"l3\": %llu, \"dram\": %llu},\n",
            (unsigned long long)t.lvl_cnt[0], (unsigned long long)t.lvl_cnt[1],
            (unsigned long long)t.lvl_cnt[2], (unsigned long long)t.lvl_cnt[3]);
    fprintf(f, "    \"lat_class_histogram\": [");
    for (int i = 0; i < 16; i++)
        fprintf(f, "%s%llu", i ? ", " : "", (unsigned long long)t.class_cnt[i]);
    fprintf(f, "],\n");
    fprintf(f, "    \"lat_class_cycles\": [");
    for (int i = 0; i < 16; i++) fprintf(f, "%s%u", i ? ", " : "", (unsigned)bstf_lat_table[i]);
    fprintf(f, "],\n");
    {
        double tot = 0; uint64_t n = 0;
        for (int i = 0; i < 16; i++) { tot += (double)t.class_cnt[i] * bstf_lat_table[i]; n += t.class_cnt[i]; }
        fprintf(f, "    \"avg_unloaded_latency_cycles\": %.4f\n", n ? tot / (double)n : 0.0);
    }
    fprintf(f, "  }");

    if (!s.sw_acc.empty()) {
        fprintf(f, ",\n  \"sweep\": {\n    \"level\": \"%s\",\n    \"variants\": [\n",
                c.sweep_level.c_str());
        for (size_t i = 0; i < s.sw_acc.size(); i++) {
            uint64_t m = s.sw_acc[i] - s.sw_hit[i];
            fprintf(f, "      {\"size\": %llu, \"ways\": %u, \"sets\": %u, \"accesses\": %llu, "
                       "\"hits\": %llu, \"misses\": %llu, \"hit_rate\": %.6f, \"mpki\": %.4f}%s\n",
                    (unsigned long long)c.sweep_sizes[i], s.sweep.c_ways[i], s.sweep.c_sets[i],
                    (unsigned long long)s.sw_acc[i], (unsigned long long)s.sw_hit[i],
                    (unsigned long long)m, ratio(s.sw_hit[i], s.sw_acc[i]),
                    kilo ? m / kilo : 0.0,
                    (i + 1 < s.sw_acc.size()) ? "," : "");
        }
        fprintf(f, "    ]\n  }");
    }
    fprintf(f, "\n}\n");
    fclose(f);
}

/* ===========================================================================
 * 10. Synthetic stream generator (self-test support)
 * =========================================================================*/
static uint64_t xs_state = 88172645463325252ULL;
static uint64_t xs_rand() {
    xs_state ^= xs_state << 13; xs_state ^= xs_state >> 7; xs_state ^= xs_state << 17;
    return xs_state;
}

static int do_gen(int argc, char** argv)
{
    /* argv[0] == "--gen" */
    if (argc < 3) {
        fprintf(stderr,
            "usage: cachesim --gen <kind> [args]\n"
            "  seq      <n> <stride> [size] [base]      sequential loads\n"
            "  loop     <footprint> <iters> [stride] [size]\n"
            "  random   <n> <range> [seed]\n"
            "  conflict <n_lines> <line_stride> <iters>\n"
            "  stridepf <n> <stride> [size]             emits a PC column\n"
            "  mix2     <n> <strideA> <strideB>         two interleaved PC streams\n"
            "  store    <n> <stride> [size]             sequential stores\n");
        return 2;
    }
    std::string k = argv[1];
    char buf[160];
    if (k == "seq" || k == "store" || k == "stridepf") {
        uint64_t n = strtoull(argv[2], 0, 0);
        uint64_t stride = (argc > 3) ? strtoull(argv[3], 0, 0) : 8;
        uint64_t sz = (argc > 4) ? strtoull(argv[4], 0, 0) : 8;
        uint64_t base = (argc > 5) ? strtoull(argv[5], 0, 0) : 0x10000000ULL;
        int is_st = (k == "store") ? 1 : 0;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t a = base + i * stride;
            if (k == "stridepf")
                snprintf(buf, sizeof buf, "%llx %llu %d 0 80001000\n",
                         (unsigned long long)a, (unsigned long long)sz, is_st);
            else
                snprintf(buf, sizeof buf, "%llx %llu %d 0\n",
                         (unsigned long long)a, (unsigned long long)sz, is_st);
            fputs(buf, stdout);
        }
        return 0;
    }
    if (k == "loop") {
        uint64_t fp = strtoull(argv[2], 0, 0);
        uint64_t it = strtoull(argv[3], 0, 0);
        uint64_t stride = (argc > 4) ? strtoull(argv[4], 0, 0) : 64;
        uint64_t sz = (argc > 5) ? strtoull(argv[5], 0, 0) : 8;
        uint64_t base = 0x10000000ULL;
        for (uint64_t r = 0; r < it; r++)
            for (uint64_t o = 0; o < fp; o += stride) {
                snprintf(buf, sizeof buf, "%llx %llu 0 0\n",
                         (unsigned long long)(base + o), (unsigned long long)sz);
                fputs(buf, stdout);
            }
        return 0;
    }
    if (k == "random") {
        uint64_t n = strtoull(argv[2], 0, 0);
        uint64_t range = strtoull(argv[3], 0, 0);
        if (argc > 4) xs_state = strtoull(argv[4], 0, 0) | 1ULL;
        uint64_t base = 0x10000000ULL;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t a = base + ((xs_rand() % range) & ~7ULL);
            snprintf(buf, sizeof buf, "%llx 8 0 0\n", (unsigned long long)a);
            fputs(buf, stdout);
        }
        return 0;
    }
    if (k == "conflict") {
        uint64_t nl = strtoull(argv[2], 0, 0);
        uint64_t stride = strtoull(argv[3], 0, 0);
        uint64_t it = strtoull(argv[4], 0, 0);
        uint64_t base = 0x10000000ULL;
        for (uint64_t r = 0; r < it; r++)
            for (uint64_t i = 0; i < nl; i++) {
                snprintf(buf, sizeof buf, "%llx 8 0 0\n",
                         (unsigned long long)(base + i * stride));
                fputs(buf, stdout);
            }
        return 0;
    }
    if (k == "mix2") {
        uint64_t n = strtoull(argv[2], 0, 0);
        uint64_t sa = strtoull(argv[3], 0, 0);
        uint64_t sb = strtoull(argv[4], 0, 0);
        uint64_t ba = 0x10000000ULL, bb = 0x40000000ULL;
        for (uint64_t i = 0; i < n; i++) {
            snprintf(buf, sizeof buf, "%llx 8 0 0 aaa0\n", (unsigned long long)(ba + i * sa));
            fputs(buf, stdout);
            snprintf(buf, sizeof buf, "%llx 8 0 0 bbb0\n", (unsigned long long)(bb + i * sb));
            fputs(buf, stdout);
        }
        return 0;
    }
    fprintf(stderr, "cachesim --gen: unknown kind '%s'\n", k.c_str());
    return 2;
}

/* ===========================================================================
 * 11. main
 * =========================================================================*/
static void usage(void)
{
    fprintf(stderr,
        "BSTM offline cache simulator\n"
        "usage: cachesim <mem-stream> <config.json> <out.mem> [options]\n"
        "       cachesim --gen <kind> [args]        (synthetic stream to stdout)\n"
        "options:\n"
        "  --icount N     instruction count used as the MPKI denominator\n"
        "  --stats PATH   stats file path (default <out.mem>.stats.json)\n"
        "  --quiet        no summary on stderr\n"
        "stream format (one access per line, '#' starts a comment):\n"
        "  <vaddr_hex> <size_bytes> <is_store 0/1> <is_ifetch 0/1> [pc_hex]\n");
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "--gen") == 0) return do_gen(argc - 1, argv + 1);
    if (argc < 4) { usage(); return 2; }

    const char* in_path  = argv[1];
    const char* cfg_path = argv[2];
    const char* out_path = argv[3];
    std::string stats_path = std::string(out_path) + ".stats.json";
    uint64_t icount = 0;
    bool quiet = false;

    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--icount") && i + 1 < argc) icount = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--stats") && i + 1 < argc) stats_path = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = true;
        else { fprintf(stderr, "cachesim: unknown option %s\n", argv[i]); usage(); return 2; }
    }

    try {
        Config cfg = load_config(cfg_path);
        if (!icount) icount = cfg.icount;
        Sim sim(cfg);

        FILE* fi = (!strcmp(in_path, "-")) ? stdin : fopen(in_path, "rb");
        if (!fi) { fprintf(stderr, "cachesim: cannot open %s\n", in_path); return 1; }
        FILE* fo = fopen(out_path, "wb");
        if (!fo) { fprintf(stderr, "cachesim: cannot create %s\n", out_path); return 1; }

        LineReader rd(fi);
        std::vector<uint8_t> obuf;
        obuf.reserve(1 << 20);
        char *b, *e;
        uint64_t lineno = 0, bad = 0;

        while (rd.next(b, e)) {
            lineno++;
            const char* p = b;
            const char* q = e;
            while (p < q && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
            if (p >= q || *p == '#') continue;
            bool ok1, ok2, ok3, ok4;
            uint64_t va   = parse_hex(p, q, ok1);
            uint64_t size = parse_dec(p, q, ok2);
            uint64_t st   = parse_dec(p, q, ok3);
            uint64_t ifq  = parse_dec(p, q, ok4);
            if (!ok1 || !ok2 || !ok3 || !ok4) {
                if (++bad <= 5)
                    fprintf(stderr, "cachesim: malformed stream line %llu (skipped)\n",
                            (unsigned long long)lineno);
                continue;
            }
            bool okpc = false;
            uint64_t pc = parse_hex(p, q, okpc);
            obuf.push_back(sim.access(va, (uint32_t)size, st != 0, ifq != 0, pc, okpc));
            if (obuf.size() >= (1u << 20)) {
                fwrite(&obuf[0], 1, obuf.size(), fo);
                obuf.clear();
            }
        }
        if (!obuf.empty()) fwrite(&obuf[0], 1, obuf.size(), fo);
        fclose(fo);
        if (fi != stdin) fclose(fi);

        write_stats(stats_path.c_str(), sim, cfg, icount);

        if (!quiet) {
            const Stats& t = sim.st;
            fprintf(stderr,
                "cachesim: %llu accesses  L1I %.4f  L1D %.4f  L2 %.4f  L3 %.4f  "
                "DTLB miss %.4f  pf(acc %.3f)\n",
                (unsigned long long)t.n_access,
                ratio(t.l1i_hit, t.l1i_acc), ratio(t.l1d_hit, t.l1d_acc),
                ratio(t.l2_hit, t.l2_acc),   ratio(t.l3_hit, t.l3_acc),
                ratio(t.dtlb_miss, t.dtlb_acc), ratio(t.pf_useful, t.pf_issued));
            if (bad) fprintf(stderr, "cachesim: %llu malformed lines skipped\n",
                             (unsigned long long)bad);
        }
    } catch (const std::exception& ex) {
        fprintf(stderr, "cachesim: %s\n", ex.what());
        return 1;
    }
    return 0;
}
