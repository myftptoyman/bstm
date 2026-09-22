/* ============================================================
 * rv64_exec.h — minimal RV64IMC + Zba/Zbb/Zbs *execution* engine
 *
 * Used only to walk wrong-path (shadow) instructions, which never commit.
 * It is validated by running it in lockstep with Spike's commit log over the
 * whole correct path: every destination-register value and every effective
 * address must match, or bstf_gen reports it.  See README §11.
 *
 * Deliberately not a simulator: no CSRs, no traps, no privilege, no FP.
 * Anything it does not understand makes exec32() return false, which
 * truncates the shadow rather than inventing behaviour.
 * ============================================================ */
#ifndef RV64_EXEC_H
#define RV64_EXEC_H
#include <stdint.h>
#include <string.h>
#include <vector>
#include <unordered_map>

/* ---------------- flat memory image ---------------- */
struct MemImage {
    struct Region { uint64_t base, size; std::vector<uint8_t> data; };
    std::vector<Region> regs;

    /* byte-granular speculative overlay: wrong-path stores land here and are
       thrown away when the shadow ends, so the architectural image stays
       exactly what Spike says it is. */
    bool spec = false;
    std::unordered_map<uint64_t, uint8_t> ov;

    void add_region(uint64_t base, uint64_t size, const uint8_t* src, uint64_t srclen) {
        Region r; r.base = base; r.size = size; r.data.assign(size, 0);
        if (src && srclen) memcpy(r.data.data(), src, srclen < size ? srclen : size);
        regs.push_back(std::move(r));
    }
    Region* find(uint64_t a, int n) {
        for (auto& r : regs)
            if (a >= r.base && a + (uint64_t)n <= r.base + r.size) return &r;
        return nullptr;
    }
    void begin_spec() { spec = true; ov.clear(); }
    void end_spec()   { spec = false; ov.clear(); }

    bool read(uint64_t a, int n, uint64_t* out) {
        Region* r = find(a, n);
        if (!r) return false;
        uint64_t v = 0;
        for (int i = n - 1; i >= 0; i--) {
            uint64_t addr = a + (uint64_t)i;
            uint8_t b;
            if (spec) {
                auto it = ov.find(addr);
                b = (it != ov.end()) ? it->second : r->data[addr - r->base];
            } else b = r->data[addr - r->base];
            v = (v << 8) | b;
        }
        *out = v; return true;
    }
    bool write(uint64_t a, int n, uint64_t v) {
        Region* r = find(a, n);
        if (!r) return false;
        for (int i = 0; i < n; i++) {
            uint8_t b = (uint8_t)(v >> (8 * i));
            uint64_t addr = a + (uint64_t)i;
            if (spec) ov[addr] = b;
            else      r->data[addr - r->base] = b;
        }
        return true;
    }
};

struct Cpu { uint64_t x[32]; uint64_t pc; };

/* result of executing one instruction */
struct ExecOut {
    int      rd;          /* -1 = none */
    uint64_t rd_val;
    uint64_t next_pc;
    bool     is_mem;
    bool     is_store;
    int      msize;       /* bytes */
    uint64_t maddr;
    bool     mem_fault;   /* address outside every mapped region */
};

/* ---------------- RVC -> 32-bit expansion ---------------- */
static inline uint32_t enc_r(unsigned op, unsigned rd, unsigned f3, unsigned rs1,
                             unsigned rs2, unsigned f7)
{ return op | (rd << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (f7 << 25); }
static inline uint32_t enc_i(unsigned op, unsigned rd, unsigned f3, unsigned rs1, int32_t imm)
{ return op | (rd << 7) | (f3 << 12) | (rs1 << 15) | ((uint32_t)(imm & 0xfff) << 20); }
static inline uint32_t enc_s(unsigned op, unsigned f3, unsigned rs1, unsigned rs2, int32_t imm)
{ uint32_t i = (uint32_t)imm;
  return op | ((i & 0x1f) << 7) | (f3 << 12) | (rs1 << 15) | (rs2 << 20) | (((i >> 5) & 0x7f) << 25); }
static inline uint32_t enc_b(unsigned op, unsigned f3, unsigned rs1, unsigned rs2, int32_t imm)
{ uint32_t i = (uint32_t)imm;
  return op | (((i >> 11) & 1) << 7) | (((i >> 1) & 0xf) << 8) | (f3 << 12) | (rs1 << 15)
            | (rs2 << 20) | (((i >> 5) & 0x3f) << 25) | (((i >> 12) & 1) << 31); }
static inline uint32_t enc_u(unsigned op, unsigned rd, int32_t imm20)
{ return op | (rd << 7) | ((uint32_t)imm20 << 12); }
static inline uint32_t enc_j(unsigned op, unsigned rd, int32_t imm)
{ uint32_t i = (uint32_t)imm;
  return op | (rd << 7) | (((i >> 12) & 0xff) << 12) | (((i >> 11) & 1) << 20)
            | (((i >> 1) & 0x3ff) << 21) | (((i >> 20) & 1) << 31); }

/* returns 0 if the compressed encoding is not supported */
static inline uint32_t rvc_expand(uint16_t c)
{
    unsigned op = c & 3, f3 = (c >> 13) & 7;
    unsigned rd = (c >> 7) & 0x1f, rs2 = (c >> 2) & 0x1f;
    unsigned rdp = 8 + ((c >> 2) & 7), rsp = 8 + ((c >> 7) & 7);

    if (op == 0) {
        switch (f3) {
        case 0: { /* c.addi4spn */
            unsigned i = (((c >> 11) & 3) << 4) | (((c >> 7) & 0xf) << 6)
                       | (((c >> 6) & 1) << 2) | (((c >> 5) & 1) << 3);
            if (!i) return 0;
            return enc_i(0x13, rdp, 0, 2, (int32_t)i);
        }
        case 2: { /* c.lw */
            unsigned i = (((c >> 10) & 7) << 3) | (((c >> 6) & 1) << 2) | (((c >> 5) & 1) << 6);
            return enc_i(0x03, rdp, 2, rsp, (int32_t)i);
        }
        case 3: { /* c.ld */
            unsigned i = (((c >> 10) & 7) << 3) | (((c >> 5) & 3) << 6);
            return enc_i(0x03, rdp, 3, rsp, (int32_t)i);
        }
        case 6: { /* c.sw */
            unsigned i = (((c >> 10) & 7) << 3) | (((c >> 6) & 1) << 2) | (((c >> 5) & 1) << 6);
            return enc_s(0x23, 2, rsp, rdp, (int32_t)i);
        }
        case 7: { /* c.sd */
            unsigned i = (((c >> 10) & 7) << 3) | (((c >> 5) & 3) << 6);
            return enc_s(0x23, 3, rsp, rdp, (int32_t)i);
        }
        default: return 0;          /* c.fld / c.fsd: no FP in this workload */
        }
    }

    if (op == 1) {
        int32_t imm6 = (int32_t)((((c >> 12) & 1) << 5) | ((c >> 2) & 0x1f));
        if (imm6 & 0x20) imm6 -= 64;
        switch (f3) {
        case 0: return enc_i(0x13, rd, 0, rd, imm6);                 /* c.addi / c.nop */
        case 1: return rd ? enc_i(0x1b, rd, 0, rd, imm6) : 0;        /* c.addiw */
        case 2: return enc_i(0x13, rd, 0, 0, imm6);                  /* c.li */
        case 3:
            if (rd == 2) {                                           /* c.addi16sp */
                int32_t i = (int32_t)((((c >> 12) & 1) << 9) | (((c >> 6) & 1) << 4)
                                    | (((c >> 5) & 1) << 6) | (((c >> 3) & 3) << 7)
                                    | (((c >> 2) & 1) << 5));
                if (i & 0x200) i -= 1024;
                return i ? enc_i(0x13, 2, 0, 2, i) : 0;
            } else {                                                 /* c.lui */
                int32_t i = (int32_t)((((c >> 12) & 1) << 5) | ((c >> 2) & 0x1f));
                if (i & 0x20) i -= 64;
                return i ? enc_u(0x37, rd, i & 0xfffff) : 0;
            }
        case 4: {
            unsigned f2 = (c >> 10) & 3;
            unsigned sh = (((c >> 12) & 1) << 5) | ((c >> 2) & 0x1f);
            if (f2 == 0) return enc_i(0x13, rsp, 5, rsp, (int32_t)sh);              /* srli */
            if (f2 == 1) return enc_i(0x13, rsp, 5, rsp, (int32_t)(sh | 0x400));    /* srai */
            if (f2 == 2) return enc_i(0x13, rsp, 7, rsp, imm6);                     /* andi */
            {
                unsigned hi = (c >> 12) & 1, lo = (c >> 5) & 3;
                if (!hi) {
                    if (lo == 0) return enc_r(0x33, rsp, 0, rsp, rdp, 0x20);  /* sub */
                    if (lo == 1) return enc_r(0x33, rsp, 4, rsp, rdp, 0x00);  /* xor */
                    if (lo == 2) return enc_r(0x33, rsp, 6, rsp, rdp, 0x00);  /* or  */
                    return              enc_r(0x33, rsp, 7, rsp, rdp, 0x00);  /* and */
                }
                if (lo == 0) return enc_r(0x3b, rsp, 0, rsp, rdp, 0x20);      /* subw */
                if (lo == 1) return enc_r(0x3b, rsp, 0, rsp, rdp, 0x00);      /* addw */
                return 0;
            }
        }
        case 5: { /* c.j */
            int32_t i = (int32_t)((((c >> 12) & 1) << 11) | (((c >> 11) & 1) << 4)
                                | (((c >> 9) & 3) << 8)  | (((c >> 8) & 1) << 10)
                                | (((c >> 7) & 1) << 6)  | (((c >> 6) & 1) << 7)
                                | (((c >> 3) & 7) << 1)  | (((c >> 2) & 1) << 5));
            if (i & 0x800) i -= 4096;
            return enc_j(0x6f, 0, i);
        }
        case 6: case 7: { /* c.beqz / c.bnez */
            int32_t i = (int32_t)((((c >> 12) & 1) << 8) | (((c >> 10) & 3) << 3)
                                | (((c >> 5) & 3) << 6)  | (((c >> 3) & 3) << 1)
                                | (((c >> 2) & 1) << 5));
            if (i & 0x100) i -= 512;
            return enc_b(0x63, f3 == 6 ? 0 : 1, rsp, 0, i);
        }
        default: return 0;
        }
    }

    /* op == 2 */
    switch (f3) {
    case 0: { unsigned sh = (((c >> 12) & 1) << 5) | ((c >> 2) & 0x1f);
              return rd ? enc_i(0x13, rd, 1, rd, (int32_t)sh) : 0; }          /* c.slli */
    case 2: { unsigned i = (((c >> 12) & 1) << 5) | (((c >> 4) & 7) << 2) | (((c >> 2) & 3) << 6);
              return rd ? enc_i(0x03, rd, 2, 2, (int32_t)i) : 0; }            /* c.lwsp */
    case 3: { unsigned i = (((c >> 12) & 1) << 5) | (((c >> 5) & 3) << 3) | (((c >> 2) & 7) << 6);
              return rd ? enc_i(0x03, rd, 3, 2, (int32_t)i) : 0; }            /* c.ldsp */
    case 4: {
        unsigned hi = (c >> 12) & 1;
        if (!hi) {
            if (rs2 == 0) return rd ? enc_i(0x67, 0, 0, rd, 0) : 0;           /* c.jr  */
            return enc_r(0x33, rd, 0, 0, rs2, 0);                             /* c.mv  */
        }
        if (rs2 == 0) {
            if (rd == 0) return 0;                                            /* c.ebreak */
            return enc_i(0x67, 1, 0, rd, 0);                                  /* c.jalr */
        }
        return enc_r(0x33, rd, 0, rd, rs2, 0);                                /* c.add */
    }
    case 6: { unsigned i = (((c >> 9) & 0xf) << 2) | (((c >> 7) & 3) << 6);
              return enc_s(0x23, 2, 2, rs2, (int32_t)i); }                    /* c.swsp */
    case 7: { unsigned i = (((c >> 10) & 7) << 3) | (((c >> 7) & 7) << 6);
              return enc_s(0x23, 3, 2, rs2, (int32_t)i); }                    /* c.sdsp */
    default: return 0;
    }
}

/* ---------------- 32-bit execution ---------------- */
static inline int64_t  sx(uint64_t v, int b) { int sh = 64 - b; return ((int64_t)(v << sh)) >> sh; }
static inline uint64_t sw32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }

static inline uint64_t rv_clz(uint64_t v, int w)
{ if (!v) return (uint64_t)w; int n = 0; for (int i = w - 1; i >= 0 && !((v >> i) & 1); i--) n++; return (uint64_t)n; }
static inline uint64_t rv_ctz(uint64_t v, int w)
{ if (!v) return (uint64_t)w; int n = 0; for (int i = 0; i < w && !((v >> i) & 1); i++) n++; return (uint64_t)n; }
static inline uint64_t rv_cpop(uint64_t v, int w)
{ uint64_t n = 0; for (int i = 0; i < w; i++) n += (v >> i) & 1; return n; }

/* Execute one already-expanded 32-bit instruction.
   Returns false for anything not modelled -- the caller must then stop. */
static inline bool exec32(Cpu& c, MemImage& m, uint32_t x, ExecOut* o, int ilen_override = 0)
{
    unsigned opc = x & 0x7f, rd = (x >> 7) & 0x1f, f3 = (x >> 12) & 7;
    unsigned rs1 = (x >> 15) & 0x1f, rs2 = (x >> 20) & 0x1f, f7 = (x >> 25) & 0x7f;
    uint64_t a = c.x[rs1], b = c.x[rs2];
    uint64_t pc = c.pc;
    int ilen = ilen_override ? ilen_override : (((x & 3) == 3) ? 4 : 2);

    o->rd = -1; o->rd_val = 0; o->next_pc = pc + (uint64_t)ilen;
    o->is_mem = false; o->is_store = false; o->msize = 0; o->maddr = 0; o->mem_fault = false;

    auto wr = [&](uint64_t v) { if (rd) { o->rd = (int)rd; o->rd_val = v; } };

    switch (opc) {
    case 0x37: wr((uint64_t)(int64_t)(int32_t)(x & 0xfffff000)); return true;               /* lui   */
    case 0x17: wr(pc + (uint64_t)(int64_t)(int32_t)(x & 0xfffff000)); return true;          /* auipc */
    case 0x6f: {                                                                            /* jal   */
        int32_t i = (int32_t)((((x >> 31) & 1) << 20) | (((x >> 21) & 0x3ff) << 1)
                            | (((x >> 20) & 1) << 11) | (((x >> 12) & 0xff) << 12));
        if (i & 0x100000) i -= 0x200000;
        wr(pc + (uint64_t)ilen); o->next_pc = pc + (uint64_t)(int64_t)i; return true;
    }
    case 0x67: {                                                                            /* jalr  */
        int64_t i = sx((x >> 20) & 0xfff, 12);
        uint64_t t = (a + (uint64_t)i) & ~(uint64_t)1;
        wr(pc + (uint64_t)ilen); o->next_pc = t; return true;
    }
    case 0x63: {                                                                            /* branch*/
        int32_t i = (int32_t)((((x >> 31) & 1) << 12) | (((x >> 25) & 0x3f) << 5)
                            | (((x >> 8) & 0xf) << 1) | (((x >> 7) & 1) << 11));
        if (i & 0x1000) i -= 0x2000;
        bool t;
        switch (f3) {
        case 0: t = (a == b); break;
        case 1: t = (a != b); break;
        case 4: t = ((int64_t)a <  (int64_t)b); break;
        case 5: t = ((int64_t)a >= (int64_t)b); break;
        case 6: t = (a <  b); break;
        case 7: t = (a >= b); break;
        default: return false;
        }
        if (t) o->next_pc = pc + (uint64_t)(int64_t)i;
        return true;
    }
    case 0x03: {                                                                            /* load  */
        int64_t i = sx((x >> 20) & 0xfff, 12);
        uint64_t ad = a + (uint64_t)i;
        static const int szt[8] = {1,2,4,8,1,2,4,0};
        int n = szt[f3]; if (!n) return false;
        o->is_mem = true; o->msize = n; o->maddr = ad;
        uint64_t v;
        if (!m.read(ad, n, &v)) { o->mem_fault = true; v = 0; }
        if (f3 < 4) v = (uint64_t)sx(v, n * 8);
        wr(v); return true;
    }
    case 0x23: {                                                                            /* store */
        int32_t i = (int32_t)(((x >> 7) & 0x1f) | (((x >> 25) & 0x7f) << 5));
        if (i & 0x800) i -= 0x1000;
        uint64_t ad = a + (uint64_t)(int64_t)i;
        if (f3 > 3) return false;
        int n = 1 << f3;
        o->is_mem = true; o->is_store = true; o->msize = n; o->maddr = ad;
        if (!m.write(ad, n, b)) o->mem_fault = true;
        return true;
    }
    case 0x13: {                                                                            /* op-imm */
        int64_t i = sx((x >> 20) & 0xfff, 12);
        unsigned sh = (x >> 20) & 0x3f, top = (x >> 26) & 0x3f;
        switch (f3) {
        case 0: wr(a + (uint64_t)i); return true;                                   /* addi  */
        case 2: wr((int64_t)a <  i ? 1 : 0); return true;                           /* slti  */
        case 3: wr(a < (uint64_t)i ? 1 : 0); return true;                           /* sltiu */
        case 4: wr(a ^ (uint64_t)i); return true;                                   /* xori  */
        case 6: wr(a | (uint64_t)i); return true;                                   /* ori   */
        case 7: wr(a & (uint64_t)i); return true;                                   /* andi  */
        case 1:
            if (top == 0x00) { wr(a << sh); return true; }                          /* slli  */
            if (top == 0x18) {                          /* Zbb unary: funct7 0x30 */
                if (rs2 == 0) { wr((uint64_t)rv_clz(a, 64)); return true; }         /* clz   */
                if (rs2 == 1) { wr((uint64_t)rv_ctz(a, 64)); return true; }         /* ctz   */
                if (rs2 == 2) { wr((uint64_t)rv_cpop(a, 64)); return true; }        /* cpop  */
                if (rs2 == 4) { wr((uint64_t)sx(a, 8));  return true; }             /* sext.b*/
                if (rs2 == 5) { wr((uint64_t)sx(a, 16)); return true; }             /* sext.h*/
                return false;
            }
            if (top == 0x12) { wr(a & ~(1ull << sh)); return true; }                /* bclri */
            if (top == 0x0a) { wr(a |  (1ull << sh)); return true; }                /* bseti */
            if (top == 0x1a) { wr(a ^  (1ull << sh)); return true; }                /* binvi */
            return false;
        case 5:
            if (top == 0x00) { wr(a >> sh); return true; }                          /* srli  */
            if (top == 0x10) { wr((uint64_t)((int64_t)a >> sh)); return true; }     /* srai  */
            if (top == 0x12) { wr((a >> sh) & 1); return true; }                    /* bexti */
            if (top == 0x18) { wr((a >> sh) | (a << ((64 - sh) & 63))); return true; } /* rori */
            if (top == 0x1a && sh == 0x38) {                                        /* rev8  */
                uint64_t v = 0; for (int k = 0; k < 8; k++) v |= ((a >> (8*k)) & 0xff) << (8*(7-k));
                wr(v); return true;
            }
            if (top == 0x0a && sh == 0x07) {                                        /* orc.b */
                uint64_t v = 0; for (int k = 0; k < 8; k++) if ((a >> (8*k)) & 0xff) v |= 0xffull << (8*k);
                wr(v); return true;
            }
            return false;
        default: return false;
        }
    }
    case 0x1b: {                                                                            /* op-imm-32 */
        int64_t i = sx((x >> 20) & 0xfff, 12);
        unsigned sh = (x >> 20) & 0x1f, top = (x >> 25) & 0x7f;
        switch (f3) {
        case 0: wr(sw32((uint32_t)(a + (uint64_t)i))); return true;                 /* addiw  */
        case 1:
            if (top == 0x00) { wr(sw32((uint32_t)a << sh)); return true; }          /* slliw  */
            if (top == 0x30) {                                                      /* Zbb w  */
                if (rs2 == 0) { wr((uint64_t)rv_clz((uint32_t)a, 32)); return true; }
                if (rs2 == 1) { wr((uint64_t)rv_ctz((uint32_t)a, 32)); return true; }
                if (rs2 == 2) { wr((uint64_t)rv_cpop((uint32_t)a, 32)); return true; }
                return false;
            }
            if (top == 0x04) { wr((a & 0xffffffffull) << sh); return true; }         /* slli.uw */
            return false;
        case 5:
            if (top == 0x00) { wr(sw32((uint32_t)a >> sh)); return true; }           /* srliw  */
            if (top == 0x20) { wr(sw32((uint32_t)((int32_t)a >> sh))); return true; }/* sraiw  */
            if (top == 0x30) { uint32_t v32 = (uint32_t)a;                           /* roriw  */
                               wr(sw32((v32 >> sh) | (v32 << ((32 - sh) & 31)))); return true; }
            return false;
        default: return false;
        }
    }
    case 0x33: {                                                                            /* op */
        if (f7 == 0x01) {                                                           /* M */
            switch (f3) {
            case 0: wr(a * b); return true;
            case 1: { __int128 r = (__int128)(int64_t)a * (__int128)(int64_t)b;
                      wr((uint64_t)(r >> 64)); return true; }
            case 2: { __int128 r = (__int128)(int64_t)a * (__int128)(unsigned __int128)b;
                      wr((uint64_t)(r >> 64)); return true; }
            case 3: { unsigned __int128 r = (unsigned __int128)a * (unsigned __int128)b;
                      wr((uint64_t)(r >> 64)); return true; }
            case 4: wr(b == 0 ? ~0ull
                     : ((int64_t)a == INT64_MIN && (int64_t)b == -1) ? a
                     : (uint64_t)((int64_t)a / (int64_t)b)); return true;
            case 5: wr(b == 0 ? ~0ull : a / b); return true;
            case 6: wr(b == 0 ? a
                     : ((int64_t)a == INT64_MIN && (int64_t)b == -1) ? 0
                     : (uint64_t)((int64_t)a % (int64_t)b)); return true;
            case 7: wr(b == 0 ? a : a % b); return true;
            default: return false;
            }
        }
        if (f7 == 0x00) {
            switch (f3) {
            case 0: wr(a + b); return true;
            case 1: wr(a << (b & 63)); return true;
            case 2: wr((int64_t)a < (int64_t)b ? 1 : 0); return true;
            case 3: wr(a < b ? 1 : 0); return true;
            case 4: wr(a ^ b); return true;
            case 5: wr(a >> (b & 63)); return true;
            case 6: wr(a | b); return true;
            case 7: wr(a & b); return true;
            default: return false;
            }
        }
        if (f7 == 0x20) {
            switch (f3) {
            case 0: wr(a - b); return true;
            case 5: wr((uint64_t)((int64_t)a >> (b & 63))); return true;
            case 4: wr(a ^ ~b); return true;                                        /* xnor */
            case 6: wr(a | ~b); return true;                                        /* orn  */
            case 7: wr(a & ~b); return true;                                        /* andn */
            default: return false;
            }
        }
        if (f7 == 0x10) {                                                           /* Zba */
            if (f3 == 2) { wr((a << 1) + b); return true; }                         /* sh1add */
            if (f3 == 4) { wr((a << 2) + b); return true; }                         /* sh2add */
            if (f3 == 6) { wr((a << 3) + b); return true; }                         /* sh3add */
            return false;
        }
        if (f7 == 0x05) {                                                           /* Zbb minmax */
            if (f3 == 4) { wr((int64_t)a < (int64_t)b ? a : b); return true; }       /* min  */
            if (f3 == 5) { wr(a < b ? a : b); return true; }                         /* minu */
            if (f3 == 6) { wr((int64_t)a > (int64_t)b ? a : b); return true; }        /* max  */
            if (f3 == 7) { wr(a > b ? a : b); return true; }                          /* maxu */
            return false;
        }
        if (f7 == 0x30) {                                                            /* rol/ror */
            unsigned sh = b & 63;
            if (f3 == 1) { wr(sh ? ((a << sh) | (a >> (64 - sh))) : a); return true; }
            if (f3 == 5) { wr(sh ? ((a >> sh) | (a << (64 - sh))) : a); return true; }
            return false;
        }
        if (f7 == 0x24) { if (f3 == 1) { wr(a & ~(1ull << (b & 63))); return true; }  /* bclr */
                          if (f3 == 5) { wr((a >> (b & 63)) & 1); return true; }      /* bext */
                          return false; }
        if (f7 == 0x14 && f3 == 1) { wr(a | (1ull << (b & 63))); return true; }       /* bset */
        if (f7 == 0x34 && f3 == 1) { wr(a ^ (1ull << (b & 63))); return true; }       /* binv */
        return false;
    }
    case 0x3b: {                                                                             /* op-32 */
        uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
        if (f7 == 0x01) {
            switch (f3) {
            case 0: wr(sw32(a32 * b32)); return true;                                /* mulw  */
            case 4: wr(b32 == 0 ? ~0ull
                     : ((int32_t)a32 == INT32_MIN && (int32_t)b32 == -1) ? sw32(a32)
                     : sw32((uint32_t)((int32_t)a32 / (int32_t)b32))); return true;   /* divw  */
            case 5: wr(b32 == 0 ? ~0ull : sw32(a32 / b32)); return true;              /* divuw */
            case 6: wr(b32 == 0 ? sw32(a32)
                     : ((int32_t)a32 == INT32_MIN && (int32_t)b32 == -1) ? 0
                     : sw32((uint32_t)((int32_t)a32 % (int32_t)b32))); return true;   /* remw  */
            case 7: wr(b32 == 0 ? sw32(a32) : sw32(a32 % b32)); return true;          /* remuw */
            default: return false;
            }
        }
        if (f7 == 0x00) {
            if (f3 == 0) { wr(sw32(a32 + b32)); return true; }                        /* addw */
            if (f3 == 1) { wr(sw32(a32 << (b & 31))); return true; }                  /* sllw */
            if (f3 == 5) { wr(sw32(a32 >> (b & 31))); return true; }                  /* srlw */
            return false;
        }
        if (f7 == 0x20) {
            if (f3 == 0) { wr(sw32(a32 - b32)); return true; }                        /* subw */
            if (f3 == 5) { wr(sw32((uint32_t)((int32_t)a32 >> (b & 31)))); return true; } /* sraw */
            return false;
        }
        if (f7 == 0x04) {
            if (f3 == 0) { wr((a & 0xffffffffull) + b); return true; }                 /* add.uw  */
            if (f3 == 4 && rs2 == 0) { wr(a & 0xffffull); return true; }               /* zext.h  */
            return false;
        }
        if (f7 == 0x10) {
            if (f3 == 2) { wr(((a & 0xffffffffull) << 1) + b); return true; }          /* sh1add.uw */
            if (f3 == 4) { wr(((a & 0xffffffffull) << 2) + b); return true; }          /* sh2add.uw */
            if (f3 == 6) { wr(((a & 0xffffffffull) << 3) + b); return true; }          /* sh3add.uw */
            return false;
        }
        if (f7 == 0x30) {                                                              /* rolw/rorw */
            unsigned sh = b & 31;
            if (f3 == 1) { wr(sw32(sh ? ((a32 << sh) | (a32 >> (32 - sh))) : a32)); return true; }
            if (f3 == 5) { wr(sw32(sh ? ((a32 >> sh) | (a32 << (32 - sh))) : a32)); return true; }
            return false;
        }
        return false;
    }
    case 0x0f: return true;                                    /* fence / fence.i: architectural nop */
    default:   return false;                                   /* SYSTEM, FP, AMO, vendor: not modelled */
    }
}

/* Expand-if-compressed, then execute. */
static inline bool exec_any(Cpu& c, MemImage& m, uint32_t raw, ExecOut* o)
{
    if ((raw & 3) == 3) return exec32(c, m, raw, o);
    uint32_t e = rvc_expand((uint16_t)raw);
    if (!e) return false;
    /* the expanded form is 4 bytes wide but the real instruction is 2, which
       matters for both the sequential next PC and the jal/jalr link value */
    return exec32(c, m, e, o, 2);
}

#endif /* RV64_EXEC_H */
