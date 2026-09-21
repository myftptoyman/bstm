// ============================================================
// bstf_gen.cc -- Spike commit-log  ->  BSTF trace triple
//
//   stdin :  output of `spike -l --log-commits ...`
//   stdout:  statistics report
//   files :  <prefix>.bstf  <prefix>.frontend.txt  <prefix>.mem.txt
//
// Agent H, BSTM project.  See README.md for the route rationale.
// ============================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <ctime>
#include "rv64_decode.h"

#define BSTF_GEN_VERSION "2.2"   /* 2.0 B1/B2/A2/A4/B3 | 2.1 G4 | 2.2 frontend next_pc */

extern "C" {
#include "bstf.h"
}

// ------------------------------------------------------------------
// fast line reader
// ------------------------------------------------------------------
namespace {

struct LineReader {
    FILE*  f;
    char*  buf;
    size_t cap, len, pos;
    explicit LineReader(FILE* fp) : f(fp), cap(1u << 22), len(0), pos(0) {
        buf = (char*)malloc(cap);
    }
    ~LineReader() { free(buf); }
    // returns pointer to a NUL-terminated line (newline stripped), or nullptr
    char* next(size_t* out_len) {
        for (;;) {
            char* nl = (char*)memchr(buf + pos, '\n', len - pos);
            if (nl) {
                char* s = buf + pos;
                *nl = '\0';
                *out_len = (size_t)(nl - s);
                pos = (size_t)(nl - buf) + 1;
                return s;
            }
            if (pos) { memmove(buf, buf + pos, len - pos); len -= pos; pos = 0; }
            if (len == cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
            size_t got = fread(buf + len, 1, cap - len, f);
            if (got == 0) {
                if (len == pos) return nullptr;
                // last line without newline
                if (len == cap) { cap += 1; buf = (char*)realloc(buf, cap); }
                buf[len] = '\0';
                *out_len = len - pos;
                char* s = buf + pos; pos = len;
                return s;
            }
            len += got;
        }
    }
};

// ------------------------------------------------------------------
// register-name table (spike prints ABI names)
// ------------------------------------------------------------------
const char* kXName[32] = {
    "zero","ra","sp","gp","tp","t0","t1","t2","s0","s1","a0","a1","a2","a3",
    "a4","a5","a6","a7","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11",
    "t3","t4","t5","t6"
};
const char* kFName[32] = {
    "ft0","ft1","ft2","ft3","ft4","ft5","ft6","ft7","fs0","fs1","fa0","fa1",
    "fa2","fa3","fa4","fa5","fa6","fa7","fs2","fs3","fs4","fs5","fs6","fs7",
    "fs8","fs9","fs10","fs11","ft8","ft9","ft10","ft11"
};
std::map<std::string,int> g_regmap;   // name -> 0..63

void build_regmap() {
    for (int i = 0; i < 32; i++) g_regmap[kXName[i]] = i;
    for (int i = 0; i < 32; i++) g_regmap[kFName[i]] = 32 + i;
    g_regmap["fp"] = 8;   // alias sometimes used for s0
}

inline uint64_t hex64(const char* p, const char** end) {
    uint64_t v = 0;
    while (*p) {
        unsigned c = (unsigned char)*p;
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;  p++;
    }
    if (end) *end = p;
    return v;
}

// ------------------------------------------------------------------
// one retired instruction as seen in the log
// ------------------------------------------------------------------
struct LogInsn {
    uint64_t pc      = 0;
    uint32_t insn    = 0;
    uint8_t  ilen    = 0;
    // register activity observed in the log
    int      rd[2]   = {-1,-1};  int nrd = 0;   // writes
    int      rs[4]   = {-1,-1,-1,-1}; int nrs = 0; // reads
    // data memory activity observed in the log
    bool     has_mem = false;
    bool     mstore  = false;
    uint64_t maddr   = 0;
    uint8_t  msize   = 0;        // bytes
    bool     csr     = false;
    void reset() { nrd = nrs = 0; has_mem = false; mstore = false; csr = false; msize = 0; }
};

// ------------------------------------------------------------------
struct Opt {
    std::string prefix   = "trace";
    std::string workload = "coremark";
    std::string isa      = "rv64imac_zba_zbb_zbs";
    uint64_t    skip     = 0;          // skip this many retires
    uint64_t    count    = 0;          // 0 = unlimited
    uint64_t    start_pc = ~0ull;      // begin at first occurrence of this PC
    uint64_t    stop_pc  = ~0ull;      // stop when this PC retires
    unsigned    fetch_width = 4;
    unsigned    line_bytes  = 64;
    bool        split_line  = true;
    unsigned    rec_bytes   = (unsigned)sizeof(bstf_rec_t);   // 16 since bstf.h v2
    uint64_t    audit_n     = 100;
    uint64_t    audit_seed  = 12345;
    std::string elf_path;                  // for the sidecar meta files
    std::string elf_sha256;
    std::string spike_cmd;
    std::string note;
};

// xorshift for audit sampling
uint64_t rnd_state;
inline uint64_t rnd() {
    rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17; return rnd_state;
}

const char* kClassName[16] = {
    "ALU","MUL","DIV","FPU","LOAD","STORE","BRANCH","JUMP",
    "JALR","RET","CSR","FENCE","AMO","NOP","SYS","VEC"
};
const char* kBrName[5] = {"cond","jal","jalr","ret","call"};

} // namespace

// ------------------------------------------------------------------
int main(int argc, char** argv)
{
    Opt o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto nx = [&]() -> const char* { return argv[++i]; };
        if      (a == "--out")         o.prefix   = nx();
        else if (a == "--workload")    o.workload = nx();
        else if (a == "--isa")         o.isa      = nx();
        else if (a == "--skip")        o.skip     = strtoull(nx(), 0, 0);
        else if (a == "--count")       o.count    = strtoull(nx(), 0, 0);
        else if (a == "--start-pc")    o.start_pc = strtoull(nx(), 0, 0);
        else if (a == "--stop-pc")     o.stop_pc  = strtoull(nx(), 0, 0);
        else if (a == "--fetch-width") o.fetch_width = (unsigned)strtoul(nx(), 0, 0);
        else if (a == "--line")        o.line_bytes  = (unsigned)strtoul(nx(), 0, 0);
        else if (a == "--no-split-line") o.split_line = false;
        else if (a == "--rec-bytes")   o.rec_bytes = (unsigned)strtoul(nx(), 0, 0);
        else if (a == "--audit")       o.audit_n  = strtoull(nx(), 0, 0);
        else if (a == "--elf")         o.elf_path = nx();
        else if (a == "--elf-sha256")  o.elf_sha256 = nx();
        else if (a == "--spike-cmd")   o.spike_cmd = nx();
        else if (a == "--note")        o.note = nx();
        else if (a == "--help") {
            fprintf(stderr,
              "usage: spike -l --log-commits ... | bstf_gen [opts]\n"
              "  --out PREFIX      output prefix (default trace)\n"
              "  --workload NAME   header workload string\n"
              "  --isa STR         header isa string\n"
              "  --skip N          drop the first N retired instructions\n"
              "  --start-pc ADDR   start at the first retire of ADDR (after --skip)\n"
              "  --count N         emit at most N instructions\n"
              "  --stop-pc ADDR    stop when ADDR retires\n"
              "  --fetch-width N   instructions per fetch block (default 4)\n"
              "  --line N          i-cache line bytes (default 64)\n"
              "  --no-split-line   allow a fetch block to span cache lines\n"
              "  --rec-bytes N     record size; must equal sizeof(bstf_rec_t) (16)\n"
              "  --audit N         write N sampled records to PREFIX.audit.txt\n"
              "  --elf PATH        source ELF path (recorded in the .meta.json sidecars)\n"
              "  --elf-sha256 HEX  sha256 of the source ELF\n"
              "  --spike-cmd STR   spike command line used to produce the log\n"
              "  --note STR        free-form note for the .meta.json sidecars\n");
            return 0;
        }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (o.rec_bytes != sizeof(bstf_rec_t)) {
        fprintf(stderr, "--rec-bytes %u disagrees with sizeof(bstf_rec_t)=%zu\n",
                o.rec_bytes, sizeof(bstf_rec_t));
        return 2;
    }
    build_regmap();
    rnd_state = o.audit_seed;

    std::string f_bstf = o.prefix + ".bstf";
    std::string f_fe   = o.prefix + ".frontend.txt";
    std::string f_mem  = o.prefix + ".mem.txt";
    std::string f_imem = o.prefix + ".imem.txt";
    std::string f_aud  = o.prefix + ".audit.txt";

    FILE* fb = fopen(f_bstf.c_str(), "wb");
    FILE* ff = fopen(f_fe.c_str(),   "wb");
    FILE* fm = fopen(f_mem.c_str(),  "wb");
    FILE* fi = fopen(f_imem.c_str(), "wb");
    FILE* fa = o.audit_n ? fopen(f_aud.c_str(), "wb") : nullptr;
    std::string f_mm = o.prefix + ".mismatch.txt";
    FILE* fmm = fopen(f_mm.c_str(), "wb");
    if (!fb || !ff || !fm || !fi) { perror("fopen"); return 1; }
    static char b1[1<<22], b2[1<<22], b3[1<<22], b4[1<<20];
    setvbuf(fb, b1, _IOFBF, sizeof b1);
    setvbuf(ff, b2, _IOFBF, sizeof b2);
    setvbuf(fm, b3, _IOFBF, sizeof b3);
    setvbuf(fi, b4, _IOFBF, sizeof b4);
    /* column headers -- one comment line, consumers skip lines starting with '#' */
    fprintf(ff, "# frontend format v2 (7 columns)\n");
    fprintf(ff, "# pc_hex is_branch br_type(0cond 1jal 2jalr 3ret 4call) taken "
                "target_hex is_block_end next_pc_hex\n");
    fprintf(ff, "# target_hex  = architectural branch target, decoded from the immediate;\n"
                "#               valid regardless of taken; 0 for non-branches.\n"
                "# next_pc_hex = PC of the next retired instruction (== col 1 of the next\n"
                "#               row); 0 on the last row = end of trace.\n");
    fprintf(fm, "# vaddr_hex size_bytes is_store is_ifetch pc_hex\n");
    fprintf(fi, "# vaddr_hex size_bytes is_store is_ifetch pc_hex   (one row per fetch block)\n");

    // reserve header space
    bstf_hdr_t hdr;
    memset(&hdr, 0, sizeof hdr);
    fwrite(&hdr, 1, sizeof hdr, fb);

    // ---------------- statistics ----------------
    uint64_t cls_cnt[16] = {0};
    uint64_t br_cnt[5]   = {0};
    uint64_t br_taken[5] = {0};
    uint64_t blk_len_hist[16] = {0};
    uint64_t n_rec = 0, n_fe = 0, n_mem = 0, n_ifetch = 0, n_straddle = 0;
    uint64_t n_load = 0, n_store = 0, n_ctrl = 0, n_taken = 0;
    uint64_t dec_bad = 0, mism_mem = 0, mism_size = 0, mism_reg = 0;
    uint64_t n_fp_dropped = 0, n_call = 0, n_ret = 0, n_coroutine = 0, mism_tgt = 0;
    std::vector<std::string> reservoir;  reservoir.reserve(o.audit_n);

    // ---------------- parser / emitter state ----------------
    LineReader lr(stdin);
    LogInsn cur, prev;
    bool have_prev = false;
    bool in_insn   = false;
    uint64_t seen  = 0;        // retires seen in the log
    uint64_t icount_start = 0;
    bool started   = false;
    bool first_mem_seen = false;
    bool finished  = false;

    // fetch-block accumulation
    unsigned blk_n = 0;
    uint64_t blk_start_pc = 0;
    uint64_t blk_last_end = 0;
    struct MemEnt { uint64_t a; unsigned sz; unsigned st; uint64_t pc; };
    std::vector<MemEnt> blk_mem;  blk_mem.reserve(8);
    std::vector<std::string> pend_fe;  // frontend lines of the current block
    pend_fe.reserve(8);

    char lbuf[256];

    // flush the finished fetch block: i-fetch record(s) then data accesses
    auto flush_block = [&](uint64_t end_pc_excl) {
        // B2: instruction fetch goes to its own file, one row per fetch block
        // (extra rows only when the block's bytes straddle a cache line).
        uint64_t l0 = blk_start_pc & ~(uint64_t)(o.line_bytes - 1);
        uint64_t l1 = (end_pc_excl - 1) & ~(uint64_t)(o.line_bytes - 1);
        for (uint64_t l = l0; ; l += o.line_bytes) {
            fprintf(fi, "%" PRIx64 " %u 0 1 %" PRIx64 "\n", l, o.line_bytes, blk_start_pc);
            n_ifetch++;
            if (l >= l1) break;
            n_straddle++;
        }
        // B1: data accesses carry the PC of the accessing instruction
        for (auto& m : blk_mem)
            fprintf(fm, "%" PRIx64 " %u %u 0 %" PRIx64 "\n", m.a, m.sz, m.st, m.pc);
        blk_mem.clear();
        for (auto& s : pend_fe) fwrite(s.data(), 1, s.size(), ff);
        pend_fe.clear();
        n_fe++;
        blk_len_hist[blk_n < 16 ? blk_n : 15]++;
        blk_n = 0;
    };

    // emit one instruction, knowing the architectural next PC
    auto emit = [&](const LogInsn& in, uint64_t next_pc, bool last) {
        rv_dec_t d; rv_decode(in.insn, &d);
        if (!d.ok) dec_bad++;

        // ---- registers: decoder is authoritative, log used as fallback ----
        int s1 = d.src1, s2 = d.src2, dst = d.dst;
        if (!d.ok) {
            s1 = in.nrs > 0 ? in.rs[0] : -1;
            s2 = in.nrs > 1 ? in.rs[1] : -1;
            dst = in.nrd > 0 ? in.rd[0] : -1;
        }

        // ---- memory: log is authoritative (address), decoder gives size ----
        bool is_mem   = in.has_mem;
        bool is_store = in.has_mem ? in.mstore : (bool)d.is_store;
        unsigned msz  = in.has_mem ? in.msize : (unsigned)(1u << d.msize_log2);
        if ((bool)d.is_mem != is_mem) mism_mem++;
        if (d.is_mem && in.has_mem && (1u << d.msize_log2) != in.msize) {
            mism_size++;
            msz = 1u << d.msize_log2;       // trust the architectural size
        }
        // Register cross-check against the spike commit log.  Spike lists the
        // reads in its own order (for stores/branches it prints rs2 before
        // rs1), so compare as multisets, not positionally.
        if (d.ok) {
            int dec[3] = { d.src1, d.src2, d.src3 }, nd = 0, dsorted[3];
            for (int k = 0; k < 3; k++) if (dec[k] >= 0) dsorted[nd++] = dec[k];
            int lsorted[4], nl = 0;
            for (int k = 0; k < in.nrs; k++) lsorted[nl++] = in.rs[k];
            std::sort(dsorted, dsorted + nd); std::sort(lsorted, lsorted + nl);
            bool bad = (nd != nl);
            for (int k = 0; !bad && k < nd; k++) if (dsorted[k] != lsorted[k]) bad = true;
            // spike suppresses a write to x0, and CSR ops log the CSR separately
            if (!bad && !in.csr) {
                int want = d.dst, got = (in.nrd > 0) ? in.rd[0] : -1;
                if (want != got) bad = true;
            }
            if (bad) {
                mism_reg++;
                if (fmm && mism_reg <= 50)
                    fprintf(fmm, "pc=%" PRIx64 " insn=%08x len=%u class=%s "
                            "decode src=[%d,%d,%d] dst=%d | log rs=[%d,%d,%d,%d] rd=[%d,%d] csr=%d\n",
                            in.pc, in.insn, in.ilen, kClassName[d.uclass],
                            d.src1, d.src2, d.src3, d.dst,
                            in.nrs>0?in.rs[0]:-1, in.nrs>1?in.rs[1]:-1,
                            in.nrs>2?in.rs[2]:-1, in.nrs>3?in.rs[3]:-1,
                            in.nrd>0?in.rd[0]:-1, in.nrd>1?in.rd[1]:-1, in.csr?1:0);
            }
        }

        // ---- control flow ----
        uint64_t seq = in.pc + in.ilen;
        bool taken   = d.is_ctrl && (next_pc != seq);
        // For PC-relative control transfers the architectural target is known
        // even when the branch is not taken -- Agent A's BTB needs that.
        // Indirect transfers (jalr / ret) are always taken, so next_pc is it.
        uint64_t tgt = 0;
        if (d.has_target)    tgt = in.pc + (int64_t)d.br_offset;
        else if (d.is_ctrl)  tgt = next_pc;
        if (d.is_ctrl && !d.is_cond) taken = true;   // unconditional
        // cross-check: when a direct branch is taken, the decoded target must
        // be exactly where spike went next.
        if (d.has_target && taken && tgt != next_pc) mism_tgt++;
        if (d.is_ctrl) { if (d.push_ras) n_call++;
                         if (d.pop_ras) n_ret++;
                         if (d.push_ras && d.pop_ras) n_coroutine++;
                         n_ctrl++; br_cnt[d.br_type]++;
                         if (taken) { n_taken++; br_taken[d.br_type]++; } }

        // ---- fetch block bookkeeping ----
        unsigned fe_delta = 0;
        if (blk_n == 0) { fe_delta = 1; blk_start_pc = in.pc; }
        blk_n++;
        blk_last_end = in.pc + in.ilen;

        bool be = false;
        if (d.is_ctrl && taken)              be = true;
        else if (blk_n >= o.fetch_width)     be = true;
        else if (next_pc != seq)             be = true;     // trap / discontinuity
        else if (o.split_line &&
                 ((next_pc & ~(uint64_t)(o.line_bytes-1)) !=
                  (blk_start_pc & ~(uint64_t)(o.line_bytes-1)))) be = true;
        if (last) be = true;

        // ---- BSTF record ----
        bstf_rec_t r; memset(&r, 0, sizeof r);
        r.uop_class = d.uclass & 0xf;
        r.exec_lat  = rv_exec_lat(d.uclass) & 0xf;
        // A4: bit7 = valid, bit[5:0] = architectural register number.
        // Demo carries integer registers only; FP operands are dropped
        // (CoreMark is a soft-float build and contains no FP instructions).
        auto enc = [&](int r) -> uint8_t {
            if (r < 0) return 0;
            if (r >= 32) { n_fp_dropped++; return 0; }
            return (uint8_t)(0x80 | (r & 0x3f));
        };
        r.src1 = enc(s1);
        r.src2 = enc(s2);
        r.dst  = enc(dst);
        uint8_t fl = 0;
        if (d.serialize) fl |= BF_SERIALIZE;
        if (d.uclass == RC_FENCE) fl |= BF_FENCE;
        if (be) fl |= BF_BLK_END;
        if (last) fl |= BF_INTERVAL_END;
        // A2 (ISA manual 2.5.1 RAS hints)
        if (d.is_ctrl) {
            if (d.push_ras) fl |= BF_CALL;
            if (d.pop_ras)  fl |= BF_RET;
        }
        r.flags = fl;
        unsigned lg = 0; while ((1u << lg) < msz && lg < 7) lg++;
        r.mem_size     = is_mem ? (lg & 7) : 0;
        r.mem_store    = is_mem && is_store;
        r.is_block_end = be;
        r.fe_index_delta  = fe_delta;
        // CONTRACT G4: .fe and .mem indices are both 0-based, with
        //   idx(i) = sum_{j<=i} delta(j) - delta(0)
        // For .fe that works out because record 0 always opens block 0
        // (delta(0) == 1).  For .mem the very first accessing instruction
        // must therefore carry delta 0 -- it *is* index 0 -- and only the
        // second and later accesses increment.  Consumers identify which
        // records consume a .mem entry from uop_class (LOAD/STORE/AMO),
        // not from the delta.
        r.mem_index_delta = (is_mem && first_mem_seen) ? 1 : 0;
        if (is_mem) first_mem_seen = true;
        r.shadow_off = 0; r.shadow_len = 0;
        fwrite(&r, 1, o.rec_bytes, fb);

        // ---- frontend text (buffered until the block is flushed) ----
        // Column 7 (next_pc) is the PC of the next instruction that actually
        // retires -- i.e. column 1 of the next row.  It is the fetch-redirect
        // ground truth: `target` is purely architectural (decoded from the
        // immediate, independent of `taken`), so a consumer cannot derive the
        // fall-through itself -- RVC makes instructions 2 or 4 bytes and the
        // length is not in this format.  0 marks the end of the trace.
        int n = snprintf(lbuf, sizeof lbuf,
                         "%" PRIx64 " %u %u %u %" PRIx64 " %u %" PRIx64 "\n",
                         in.pc, d.is_ctrl ? 1u : 0u, d.is_ctrl ? (unsigned)d.br_type : 0u,
                         taken ? 1u : 0u, tgt, be ? 1u : 0u,
                         last ? (uint64_t)0 : next_pc);
        pend_fe.emplace_back(lbuf, (size_t)n);

        if (is_mem) { blk_mem.push_back({in.maddr, msz, is_store ? 1u : 0u, in.pc});
                      n_mem++; if (is_store) n_store++; else n_load++; }

        cls_cnt[d.uclass]++;
        n_rec++;

        // ---- audit sample (reservoir of o.audit_n records) ----
        if (o.audit_n) {
            char ab[512];
            int an = snprintf(ab, sizeof ab,
                "#%" PRIu64 " pc=%" PRIx64 " insn=%08x len=%u class=%s lat=%u "
                "src1=%d src2=%d dst=%d mem=%d store=%d size=%u addr=%" PRIx64
                " ctrl=%d br=%s taken=%d tgt=%" PRIx64 " blkend=%d | "
                "spike-log: rd=[%d,%d] rs=[%d,%d,%d] mem=%d store=%d size=%u\n",
                n_rec, in.pc, in.insn, in.ilen, kClassName[d.uclass],
                rv_exec_lat(d.uclass), s1, s2, dst, is_mem ? 1 : 0,
                is_store ? 1 : 0, is_mem ? msz : 0u, is_mem ? in.maddr : (uint64_t)0,
                d.is_ctrl ? 1 : 0, kBrName[d.br_type], taken ? 1 : 0, tgt, be ? 1 : 0,
                in.nrd > 0 ? in.rd[0] : -1, in.nrd > 1 ? in.rd[1] : -1,
                in.nrs > 0 ? in.rs[0] : -1, in.nrs > 1 ? in.rs[1] : -1,
                in.nrs > 2 ? in.rs[2] : -1,
                in.has_mem ? 1 : 0, in.mstore ? 1 : 0, in.msize);
            (void)an;
            if (reservoir.size() < o.audit_n) reservoir.emplace_back(ab);
            else {
                uint64_t j = rnd() % n_rec;          // n_rec already incremented
                if (j < o.audit_n) reservoir[j] = ab;
            }
        }

        if (be) flush_block(blk_last_end);
    };

    // ---------------- main parse loop ----------------
    size_t llen;
    char* ln;
    while (!finished && (ln = lr.next(&llen)) != nullptr) {
        if (llen == 0) continue;

        if (ln[0] == 'c' && !memcmp(ln, "core", 4)) {
            // "core   N: mem[0xPC] == 0xINSN (disasm), MODE \t#SEQ"
            const char* p = strstr(ln, "mem[0x");
            if (!p) continue;                       // ">>>> symbol" annotation
            p += 6;
            const char* e;
            uint64_t pc = hex64(p, &e);
            if (strncmp(e, "] == 0x", 7)) continue;
            uint64_t iw = hex64(e + 7, &e);

            // close out the previous instruction
            if (in_insn) {
                if (have_prev) {
                    bool is_last = (o.count && n_rec + 1 >= o.count) ||
                                   (prev.pc == o.stop_pc);
                    emit(prev, cur.pc, is_last);
                    if (is_last) finished = true;
                }
                prev = cur; have_prev = true;
                if (finished) break;
            }
            seen++;
            cur.reset();
            cur.pc   = pc;
            cur.insn = (uint32_t)iw;
            cur.ilen = (uint8_t)rv_ilen(cur.insn);
            if (cur.ilen == 2) cur.insn &= 0xffff;
            in_insn = true;

            if (!started) {
                bool ok_skip = (seen > o.skip);
                bool ok_pc   = (o.start_pc == ~0ull) || (pc == o.start_pc);
                if (ok_skip && ok_pc) { started = true; icount_start = seen - 1; }
            }
            if (!started) { in_insn = false; have_prev = false; }
            continue;
        }

        if (!in_insn || !started) continue;

        // indented detail line
        const char* p = ln;
        while (*p == ' ') p++;
        if (p[0] == 'm' && !memcmp(p, "mem[0x", 6)) {
            const char* e;
            uint64_t a = hex64(p + 6, &e);
            if (e[0] != ']') continue;
            bool st;
            if (!memcmp(e, "] <= 0x", 7)) st = true;
            else if (!memcmp(e, "] == 0x", 7)) st = false;
            else continue;
            const char* v = e + 7; const char* ve;
            hex64(v, &ve);
            unsigned digits = (unsigned)(ve - v);
            unsigned sz = digits / 2; if (sz == 0) sz = 1;
            cur.has_mem = true; cur.maddr = a; cur.mstore = st; cur.msize = sz;
            continue;
        }
        // register / CSR line:  "<name> <= 0x..."  or  "<name>(0xNNN) == 0x..."
        {
            const char* q = p;
            while (*q && *q != ' ' && *q != '(') q++;
            bool is_csr = (*q == '(');
            if (is_csr) { cur.csr = true; continue; }
            std::string nm(p, (size_t)(q - p));
            while (*q == ' ') q++;
            bool wr;
            if (!memcmp(q, "<=", 2)) wr = true;
            else if (!memcmp(q, "==", 2)) wr = false;
            else continue;
            auto it = g_regmap.find(nm);
            if (it == g_regmap.end()) continue;
            if (wr) { if (cur.nrd < 2) cur.rd[cur.nrd++] = it->second; }
            else    { if (cur.nrs < 4) cur.rs[cur.nrs++] = it->second; }
        }
    }

    // drain
    if (!finished) {
        if (in_insn) {
            if (have_prev) emit(prev, cur.pc, false);
            prev = cur; have_prev = true;
        }
        if (have_prev) emit(prev, prev.pc + prev.ilen, true);
    }
    if (blk_n) flush_block(blk_last_end);

    // ---------------- header ----------------
    hdr.magic       = BSTF_MAGIC;
    hdr.version     = BSTF_VERSION;
    hdr.rec_bytes   = o.rec_bytes;
    hdr.n_records   = n_rec;
    hdr.shadow_bytes  = 0;
    hdr.shadow_offset = 0;
    hdr.n_fe_blocks = n_fe;
    hdr.n_mem_access = n_mem;
    hdr.simpoint_id = 0;
    hdr.simpoint_weight = 65536;      // Q16 1.0
    hdr.icount_start = icount_start;
    snprintf(hdr.workload, sizeof hdr.workload, "%s", o.workload.c_str());
    snprintf(hdr.isa, sizeof hdr.isa, "%s", o.isa.c_str());
    fseek(fb, 0, SEEK_SET);
    fwrite(&hdr, 1, sizeof hdr, fb);
    fclose(fb); fclose(ff); fclose(fm); fclose(fi);
    if (fmm) fclose(fmm);
    if (fa) {
        fprintf(fa, "# %zu randomly sampled BSTF records, cross-checked against the\n"
                    "# spike commit log fields that produced them.\n", reservoir.size());
        for (auto& s2 : reservoir) fwrite(s2.data(), 1, s2.size(), fa);
        fclose(fa);
    }

    // ---------------- B3: sidecar meta files ----------------
    {
        auto isostamp = []() {
            time_t t = time(nullptr); struct tm tmv; gmtime_r(&t, &tmv);
            char b[64]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tmv);
            return std::string(b);
        };
        std::string stamp = isostamp();
        char cfg[512];
        snprintf(cfg, sizeof cfg,
                 "fetch_width=%u,line_bytes=%u,split_line=%d,rec_bytes=%u,"
                 "skip=%" PRIu64 ",start_pc=0x%" PRIx64 ",stop_pc=0x%" PRIx64 ",count=%" PRIu64,
                 o.fetch_width, o.line_bytes, o.split_line ? 1 : 0, o.rec_bytes,
                 o.skip, o.start_pc, o.stop_pc, o.count);

        struct Out { const char* name; const char* kind; const char* fmt;
                     uint64_t rows; unsigned fmtver; };
        Out outs[4] = {
            { f_bstf.c_str(), "bstf",     "binary: bstf_hdr_t header then n_records records of hdr.rec_bytes", n_rec, 2 },
            { f_fe.c_str(),   "frontend", "text: pc_hex is_branch br_type taken target_hex is_block_end next_pc_hex", n_rec, 2 },
            { f_mem.c_str(),  "dmem",     "text: vaddr_hex size_bytes is_store is_ifetch pc_hex (data only)", n_mem, 1 },
            { f_imem.c_str(), "imem",     "text: vaddr_hex size_bytes is_store is_ifetch pc_hex (one row per fetch block)", n_ifetch, 1 },
        };
        for (auto& ou : outs) {
            std::string mp = std::string(ou.name) + ".meta.json";
            FILE* mf = fopen(mp.c_str(), "wb");
            if (!mf) { perror(mp.c_str()); continue; }
            fprintf(mf,
"{\n"
"  \"file\": \"%s\",\n"
"  \"kind\": \"%s\",\n"
"  \"format\": \"%s\",\n"
"  \"format_version\": %u,\n"
"  \"rows\": %" PRIu64 ",\n"
"  \"bstf_version\": %u,\n"
"  \"bstf_rec_bytes\": %u,\n"
"  \"bstf_hdr_bytes\": %zu,\n"
"  \"sizeof_bstf_rec_t\": %zu,\n"
"  \"generator\": { \"tool\": \"bstf_gen\", \"version\": \"%s\", \"agent\": \"H\" },\n"
"  \"generated_utc\": \"%s\",\n"
"  \"workload\": \"%s\",\n"
"  \"isa\": \"%s\",\n"
"  \"source_elf\": \"%s\",\n"
"  \"source_elf_sha256\": \"%s\",\n"
"  \"spike_cmd\": \"%s\",\n"
"  \"icount_start\": %" PRIu64 ",\n"
"  \"icount_end\": %" PRIu64 ",\n"
"  \"n_records\": %" PRIu64 ",\n"
"  \"n_fe_blocks\": %" PRIu64 ",\n"
"  \"n_mem_access\": %" PRIu64 ",\n"
"  \"n_ifetch_rows\": %" PRIu64 ",\n"
"  \"config_fingerprint\": \"%s\",\n"
"  \"shadow\": { \"off\": 0, \"len\": 0, \"note\": \"wrong-path expansion deferred to the next round\" },\n"
"  \"note\": \"%s\"\n"
"}\n",
                ou.name, ou.kind, ou.fmt, ou.fmtver, ou.rows,
                (unsigned)BSTF_VERSION, o.rec_bytes, sizeof(bstf_hdr_t), sizeof(bstf_rec_t),
                BSTF_GEN_VERSION, stamp.c_str(),
                o.workload.c_str(), o.isa.c_str(),
                o.elf_path.c_str(), o.elf_sha256.c_str(), o.spike_cmd.c_str(),
                icount_start, icount_start + n_rec, n_rec, n_fe, n_mem, n_ifetch,
                cfg, o.note.c_str());
            fclose(mf);
        }
    }

    // ---------------- report ----------------
    double N = (double)(n_rec ? n_rec : 1);
    printf("=== BSTF generation report: %s ===\n", o.prefix.c_str());
    printf("retires seen in log      : %" PRIu64 "\n", seen);
    printf("icount_start             : %" PRIu64 "\n", icount_start);
    printf("records written          : %" PRIu64 "  (%u B each, hdr %zu B)\n",
           n_rec, o.rec_bytes, sizeof(bstf_hdr_t));
    printf("fetch blocks             : %" PRIu64 "  (avg %.3f insn/block)\n",
           n_fe, n_fe ? (double)n_rec / (double)n_fe : 0.0);
    printf("data mem accesses        : %" PRIu64 "  (%.2f%% of insns)\n",
           n_mem, 100.0 * (double)n_mem / N);
    printf("  sum mem_index_delta    : %" PRIu64 "  (G4: == n_mem_access - 1)\n",
           n_mem ? n_mem - 1 : 0);
    printf("  loads                  : %" PRIu64 "  (%.2f%%)\n", n_load, 100.0*(double)n_load/N);
    printf("  stores                 : %" PRIu64 "  (%.2f%%)\n", n_store, 100.0*(double)n_store/N);
    printf("i-fetch rows (%s) : %" PRIu64 "  (line-straddle extras %" PRIu64 ")\n",
           "imem.txt", n_ifetch, n_straddle);
    printf("control transfers        : %" PRIu64 "  (%.2f%%), taken %" PRIu64 " (%.2f%% of ctrl)\n",
           n_ctrl, 100.0*(double)n_ctrl/N, n_taken, n_ctrl ? 100.0*(double)n_taken/(double)n_ctrl : 0.0);
    printf("\n-- uop class mix --\n");
    for (int i = 0; i < 16; i++)
        if (cls_cnt[i]) printf("  %-7s %12" PRIu64 "  %6.2f%%\n",
                               kClassName[i], cls_cnt[i], 100.0*(double)cls_cnt[i]/N);
    printf("RAS hints                : call %" PRIu64 " (%.2f%%), ret %" PRIu64 " (%.2f%%), co-routine %" PRIu64 "\n",
           n_call, 100.0*(double)n_call/N, n_ret, 100.0*(double)n_ret/N, n_coroutine);
    printf("FP operands dropped      : %" PRIu64 " (A4: demo carries integer regs only)\n", n_fp_dropped);
    printf("\n-- control transfer mix --\n");
    for (int i = 0; i < 5; i++)
        if (br_cnt[i]) printf("  %-5s %12" PRIu64 "  %6.2f%% of insn   taken %6.2f%%\n",
                              kBrName[i], br_cnt[i], 100.0*(double)br_cnt[i]/N,
                              100.0*(double)br_taken[i]/(double)br_cnt[i]);
    printf("\n-- fetch block length histogram --\n");
    for (int i = 1; i < 16; i++)
        if (blk_len_hist[i]) printf("  %2d insn : %12" PRIu64 "  %6.2f%%\n",
                                    i, blk_len_hist[i], 100.0*(double)blk_len_hist[i]/(double)(n_fe?n_fe:1));
    printf("\n-- self-check (decoder vs spike commit log) --\n");
    printf("  unrecognised encodings : %" PRIu64 "\n", dec_bad);
    printf("  mem-presence mismatches: %" PRIu64 "\n", mism_mem);
    printf("  mem-size   mismatches  : %" PRIu64 "\n", mism_size);
    printf("  register   mismatches  : %" PRIu64 "\n", mism_reg);
    printf("  branch-target mismatches: %" PRIu64 "  (decoded PC-rel target vs spike next PC)\n", mism_tgt);
    printf("  audit samples written  : %zu -> %s\n", reservoir.size(), f_aud.c_str());
    return 0;
}
