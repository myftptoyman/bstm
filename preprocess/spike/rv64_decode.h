/* ============================================================
 * rv64_decode.h — minimal RV64GC (+Zba/Zbb/Zbc/Zbs) decoder for BSTF
 *
 * Only decodes what BSTF needs: uop class, architectural source /
 * destination registers, memory access size, and control-transfer type.
 * It deliberately does NOT evaluate anything -- values come from the
 * Spike commit log.
 *
 * Register numbering used throughout:  0..31 = x0..x31, 32..63 = f0..f31,
 * -1 = "no register".
 * ============================================================ */
#ifndef RV64_DECODE_H
#define RV64_DECODE_H
#include <stdint.h>

/* uop classes -- kept numerically identical to include/bstf.h */
enum {
    RC_ALU=0, RC_MUL=1, RC_DIV=2, RC_FPU=3, RC_LOAD=4, RC_STORE=5,
    RC_BRANCH=6, RC_JUMP=7, RC_JALR=8, RC_RET=9, RC_CSR=10,
    RC_FENCE=11, RC_AMO=12, RC_NOP=13, RC_SYS=14, RC_VEC=15
};

/* branch type used by the frontend text trace */
enum { BT_COND=0, BT_JAL=1, BT_JALR=2, BT_RET=3, BT_CALL=4 };

typedef struct {
    uint8_t  uclass;
    int8_t   src1, src2, src3;   /* -1 = none */
    int8_t   dst;                /* -1 = none */
    uint8_t  ilen;               /* 2 or 4 */
    uint8_t  is_mem;
    uint8_t  is_store;
    uint8_t  msize_log2;         /* log2(bytes) */
    uint8_t  is_ctrl;            /* any control transfer */
    uint8_t  is_cond;            /* conditional branch */
    uint8_t  br_type;            /* BT_* (valid when is_ctrl) */
    uint8_t  push_ras;           /* RISC-V ISA manual 2.5.1 -> BF_CALL */
    uint8_t  pop_ras;            /*                         -> BF_RET  */
    uint8_t  has_target;         /* 1 = PC-relative, target = pc + br_offset */
    int32_t  br_offset;          /* signed byte displacement */
    uint8_t  serialize;
    uint8_t  ok;                 /* 0 = unrecognised encoding */
} rv_dec_t;

static inline int rv_ilen(uint32_t x) { return ((x & 3) == 3) ? 4 : 2; }
static inline int8_t rv_nz(int r) { return (int8_t)(r == 0 ? -1 : r); }
static inline int rv_is_link(unsigned r) { return r == 1 || r == 5; }

/* sign-extend the low `bits` of v */
static inline int32_t rv_sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}
/* B-type branch displacement */
static inline int32_t rv_imm_b(uint32_t x)
{
    uint32_t i = (((x >> 31) & 1) << 12) | (((x >> 25) & 0x3f) << 5) |
                 (((x >> 8)  & 0xf) << 1) | (((x >> 7) & 1) << 11);
    return rv_sext(i, 13);
}
/* J-type jump displacement */
static inline int32_t rv_imm_j(uint32_t x)
{
    uint32_t i = (((x >> 31) & 1) << 20) | (((x >> 21) & 0x3ff) << 1) |
                 (((x >> 20) & 1) << 11) | (((x >> 12) & 0xff) << 12);
    return rv_sext(i, 21);
}
/* CJ-type (c.j / c.jal) displacement */
static inline int32_t rv_imm_cj(uint32_t x)
{
    uint32_t i = (((x >> 12) & 1) << 11) | (((x >> 11) & 1) << 4) |
                 (((x >> 9)  & 3) << 8)  | (((x >> 8)  & 1) << 10) |
                 (((x >> 7)  & 1) << 6)  | (((x >> 6)  & 1) << 7) |
                 (((x >> 3)  & 7) << 1)  | (((x >> 2)  & 1) << 5);
    return rv_sext(i, 12);
}
/* CB-type (c.beqz / c.bnez) displacement */
static inline int32_t rv_imm_cb(uint32_t x)
{
    uint32_t i = (((x >> 12) & 1) << 8) | (((x >> 10) & 3) << 3) |
                 (((x >> 5)  & 3) << 6) | (((x >> 3)  & 3) << 1) |
                 (((x >> 2)  & 1) << 5);
    return rv_sext(i, 9);
}

/* RISC-V ISA manual 2.5.1 return-address-stack hints for JALR.
   Sets push/pop and the resulting BSTF class + frontend br_type. */
static inline void rv_jalr_ras(unsigned rd, unsigned rs1, rv_dec_t *d)
{
    int rdl = rv_is_link(rd), r1l = rv_is_link(rs1);
    if (rdl && r1l) { d->push_ras = 1; if (rd != rs1) d->pop_ras = 1; }
    else if (rdl)   { d->push_ras = 1; }
    else if (r1l)   { d->pop_ras  = 1; }
    if (d->push_ras)      { d->uclass = RC_JALR; d->br_type = BT_CALL; }
    else if (d->pop_ras)  { d->uclass = RC_RET;  d->br_type = BT_RET;  }
    else                  { d->uclass = RC_JALR; d->br_type = BT_JALR; }
}

/* ---------- exec latency table (from the BSTM demo spec) ---------- */
static inline uint8_t rv_exec_lat(uint8_t uclass)
{
    switch (uclass) {
    case RC_MUL: return 3;
    case RC_DIV: return 12;
    case RC_FPU: return 4;
    default:     return 1;   /* ALU / LOAD / STORE / BRANCH / JUMP / ... */
    }
}

/* ---------------- compressed (RVC) ---------------- */
static void rv_decode_c(uint32_t x, rv_dec_t *d)
{
    unsigned op  = x & 3;
    unsigned f3  = (x >> 13) & 7;
    unsigned rd  = (x >> 7) & 0x1f;          /* CR/CI format */
    unsigned rs2 = (x >> 2) & 0x1f;
    unsigned rdp = 8 + ((x >> 2) & 7);       /* rd'  / rs2' */
    unsigned rsp = 8 + ((x >> 7) & 7);       /* rs1' */

    d->uclass = RC_ALU; d->ok = 1;

    if (op == 0) {
        switch (f3) {
        case 0: /* c.addi4spn */
            if (((x >> 5) & 0xff) == 0) { d->ok = 0; return; }
            d->src1 = 2; d->dst = (int8_t)rdp; return;
        case 1: /* c.fld */
            d->uclass = RC_LOAD; d->is_mem = 1; d->msize_log2 = 3;
            d->src1 = (int8_t)rsp; d->dst = (int8_t)(32 + rdp); return;
        case 2: /* c.lw */
            d->uclass = RC_LOAD; d->is_mem = 1; d->msize_log2 = 2;
            d->src1 = (int8_t)rsp; d->dst = (int8_t)rdp; return;
        case 3: /* c.ld (RV64) */
            d->uclass = RC_LOAD; d->is_mem = 1; d->msize_log2 = 3;
            d->src1 = (int8_t)rsp; d->dst = (int8_t)rdp; return;
        case 5: /* c.fsd */
            d->uclass = RC_STORE; d->is_mem = 1; d->is_store = 1; d->msize_log2 = 3;
            d->src1 = (int8_t)rsp; d->src2 = (int8_t)(32 + rdp); return;
        case 6: /* c.sw */
            d->uclass = RC_STORE; d->is_mem = 1; d->is_store = 1; d->msize_log2 = 2;
            d->src1 = (int8_t)rsp; d->src2 = (int8_t)rdp; return;
        case 7: /* c.sd */
            d->uclass = RC_STORE; d->is_mem = 1; d->is_store = 1; d->msize_log2 = 3;
            d->src1 = (int8_t)rsp; d->src2 = (int8_t)rdp; return;
        default: d->ok = 0; return;
        }
    }

    if (op == 1) {
        switch (f3) {
        case 0: /* c.nop / c.addi */
            if (rd == 0) { d->uclass = RC_NOP; return; }
            d->src1 = rv_nz(rd); d->dst = (int8_t)rd; return;
        case 1: /* c.addiw (RV64) */
            if (rd == 0) { d->ok = 0; return; }
            d->src1 = rv_nz(rd); d->dst = (int8_t)rd; return;
        case 2: /* c.li */
            d->dst = rv_nz(rd); if (d->dst < 0) d->uclass = RC_NOP; return;
        case 3: /* c.addi16sp / c.lui */
            if (rd == 2) { d->src1 = 2; d->dst = 2; }
            else         { d->dst = rv_nz(rd); if (d->dst < 0) d->uclass = RC_NOP; }
            return;
        case 4: { /* MISC-ALU */
            unsigned f2 = (x >> 10) & 3;
            if (f2 == 3) {
                unsigned f2b = (x >> 5) & 3, b12 = (x >> 12) & 1;
                (void)f2b; (void)b12;
                d->src1 = (int8_t)rsp; d->src2 = (int8_t)rdp; d->dst = (int8_t)rsp;
                return;                       /* c.sub/xor/or/and/subw/addw */
            }
            d->src1 = (int8_t)rsp; d->dst = (int8_t)rsp; return;  /* srli/srai/andi */
        }
        case 5: /* c.j */
            d->uclass = RC_JUMP; d->is_ctrl = 1; d->br_type = BT_JAL;
            d->has_target = 1; d->br_offset = rv_imm_cj(x); return;
        case 6: /* c.beqz */
        case 7: /* c.bnez */
            d->uclass = RC_BRANCH; d->is_ctrl = 1; d->is_cond = 1;
            d->br_type = BT_COND; d->src1 = (int8_t)rsp;
            d->has_target = 1; d->br_offset = rv_imm_cb(x); return;
        default: d->ok = 0; return;
        }
    }

    /* op == 2 */
    switch (f3) {
    case 0: /* c.slli */
        d->src1 = rv_nz(rd); d->dst = rv_nz(rd);
        if (d->dst < 0) d->uclass = RC_NOP;
        return;
    case 1: /* c.fldsp */
        d->uclass = RC_LOAD; d->is_mem = 1; d->msize_log2 = 3;
        d->src1 = 2; d->dst = (int8_t)(32 + rd); return;
    case 2: /* c.lwsp */
        d->uclass = RC_LOAD; d->is_mem = 1; d->msize_log2 = 2;
        d->src1 = 2; d->dst = rv_nz(rd); return;
    case 3: /* c.ldsp */
        d->uclass = RC_LOAD; d->is_mem = 1; d->msize_log2 = 3;
        d->src1 = 2; d->dst = rv_nz(rd); return;
    case 4: {
        unsigned b12 = (x >> 12) & 1;
        if (!b12) {
            if (rs2 == 0) {                   /* c.jr  ==  jalr x0, rs1, 0 */
                d->is_ctrl = 1; d->src1 = rv_nz(rd);
                rv_jalr_ras(0, rd, d);
                return;
            }
            d->src1 = (int8_t)rs2; d->dst = rv_nz(rd);   /* c.mv */
            if (d->dst < 0) d->uclass = RC_NOP;
            return;
        }
        if (rs2 == 0) {
            if (rd == 0) { d->uclass = RC_SYS; d->serialize = 1; return; }  /* c.ebreak */
            d->is_ctrl = 1;                                                 /* c.jalr */
            d->src1 = (int8_t)rd; d->dst = 1;
            rv_jalr_ras(1, rd, d); return;
        }
        d->src1 = rv_nz(rd); d->src2 = (int8_t)rs2; d->dst = rv_nz(rd);     /* c.add */
        if (d->dst < 0) d->uclass = RC_NOP;
        return;
    }
    case 5: /* c.fsdsp */
        d->uclass = RC_STORE; d->is_mem = 1; d->is_store = 1; d->msize_log2 = 3;
        d->src1 = 2; d->src2 = (int8_t)(32 + rs2); return;
    case 6: /* c.swsp */
        d->uclass = RC_STORE; d->is_mem = 1; d->is_store = 1; d->msize_log2 = 2;
        d->src1 = 2; d->src2 = rv_nz(rs2); return;   /* rs2 may be x0 */
    case 7: /* c.sdsp */
        d->uclass = RC_STORE; d->is_mem = 1; d->is_store = 1; d->msize_log2 = 3;
        d->src1 = 2; d->src2 = rv_nz(rs2); return;   /* rs2 may be x0 */
    default: d->ok = 0; return;
    }
}

/* ---------------- 32-bit ---------------- */
static void rv_decode_32(uint32_t x, rv_dec_t *d)
{
    unsigned opc = x & 0x7f;
    unsigned rd  = (x >> 7) & 0x1f;
    unsigned f3  = (x >> 12) & 7;
    unsigned rs1 = (x >> 15) & 0x1f;
    unsigned rs2 = (x >> 20) & 0x1f;
    unsigned f7  = (x >> 25) & 0x7f;

    d->uclass = RC_ALU; d->ok = 1;

    switch (opc) {
    case 0x37: case 0x17:                    /* LUI / AUIPC */
        d->dst = rv_nz(rd); if (d->dst < 0) d->uclass = RC_NOP; return;

    case 0x6f:                               /* JAL */
        d->uclass = RC_JUMP; d->is_ctrl = 1; d->dst = rv_nz(rd);
        if (rv_is_link(rd)) { d->push_ras = 1; d->br_type = BT_CALL; }
        else                {                  d->br_type = BT_JAL;  }
        d->has_target = 1; d->br_offset = rv_imm_j(x);
        return;

    case 0x67:                               /* JALR */
        d->is_ctrl = 1; d->src1 = rv_nz(rs1); d->dst = rv_nz(rd);
        rv_jalr_ras(rd, rs1, d);
        return;

    case 0x63:                               /* BRANCH (+ Andes beqi/bnei share opc? no) */
        d->uclass = RC_BRANCH; d->is_ctrl = 1; d->is_cond = 1; d->br_type = BT_COND;
        d->src1 = rv_nz(rs1); d->src2 = rv_nz(rs2);
        d->has_target = 1; d->br_offset = rv_imm_b(x);
        return;

    case 0x03:                               /* LOAD */
        d->uclass = RC_LOAD; d->is_mem = 1; d->src1 = rv_nz(rs1); d->dst = rv_nz(rd);
        d->msize_log2 = (uint8_t)(f3 & 3);
        if (f3 > 6) d->ok = 0;
        return;

    case 0x23:                               /* STORE */
        d->uclass = RC_STORE; d->is_mem = 1; d->is_store = 1;
        d->src1 = rv_nz(rs1); d->src2 = rv_nz(rs2);
        d->msize_log2 = (uint8_t)(f3 & 3);
        if (f3 > 3) d->ok = 0;
        return;

    case 0x13: case 0x1b:                    /* OP-IMM / OP-IMM-32 */
        if (x == 0x13) { d->uclass = RC_NOP; return; }
        d->src1 = rv_nz(rs1); d->dst = rv_nz(rd);
        if (d->dst < 0) d->uclass = RC_NOP;
        return;

    case 0x33: case 0x3b:                    /* OP / OP-32 */
        d->src1 = rv_nz(rs1); d->src2 = rv_nz(rs2); d->dst = rv_nz(rd);
        if (f7 == 0x01) {                    /* M extension */
            if (opc == 0x33) d->uclass = (f3 < 4) ? RC_MUL : RC_DIV;
            else             d->uclass = (f3 == 0) ? RC_MUL : RC_DIV;   /* mulw / div*w rem*w */
        }
        if (d->dst < 0 && d->uclass == RC_ALU) d->uclass = RC_NOP;
        return;

    case 0x0f:                               /* MISC-MEM: fence / fence.i / cbo.* */
        d->uclass = RC_FENCE; d->serialize = 1;
        if (f3 == 2) { d->src1 = rv_nz(rs1); }                       /* cbo.* */
        return;

    case 0x73:                               /* SYSTEM */
        if (f3 == 0) { d->uclass = RC_SYS; d->serialize = 1;
                       d->src1 = rv_nz(rs1); d->src2 = rv_nz(rs2); return; }
        d->uclass = RC_CSR; d->serialize = 1; d->dst = rv_nz(rd);
        if (f3 == 1 || f3 == 2 || f3 == 3) d->src1 = rv_nz(rs1);     /* csrrw/s/c */
        return;

    case 0x2f: {                             /* AMO / LR / SC */
        unsigned f5 = (x >> 27) & 0x1f;
        d->uclass = RC_AMO; d->is_mem = 1;
        d->msize_log2 = (uint8_t)(f3 & 7);
        d->src1 = rv_nz(rs1); d->dst = rv_nz(rd);
        if (f5 == 0x02) { d->is_store = 0; }                         /* lr */
        else            { d->is_store = 1; d->src2 = rv_nz(rs2); }
        return;
    }

    case 0x07:                               /* LOAD-FP */
        d->uclass = RC_LOAD; d->is_mem = 1; d->src1 = rv_nz(rs1);
        d->dst = (int8_t)(32 + rd);
        d->msize_log2 = (f3 == 1) ? 1 : (f3 == 2) ? 2 : (f3 == 3) ? 3 : 4;
        return;

    case 0x27:                               /* STORE-FP */
        d->uclass = RC_STORE; d->is_mem = 1; d->is_store = 1;
        d->src1 = rv_nz(rs1); d->src2 = (int8_t)(32 + rs2);
        d->msize_log2 = (f3 == 1) ? 1 : (f3 == 2) ? 2 : (f3 == 3) ? 3 : 4;
        return;

    case 0x43: case 0x47: case 0x4b: case 0x4f:   /* FMADD family */
        d->uclass = RC_FPU;
        d->src1 = (int8_t)(32 + rs1); d->src2 = (int8_t)(32 + rs2);
        d->src3 = (int8_t)(32 + ((x >> 27) & 0x1f));
        d->dst  = (int8_t)(32 + rd);
        return;

    case 0x53: {                             /* OP-FP */
        unsigned op5 = f7 >> 2;
        d->uclass = RC_FPU;
        int dst_int = (op5 == 0x14 || op5 == 0x18 || op5 == 0x1c);
        int s1_int  = (op5 == 0x1a || op5 == 0x1e);
        int has_rs2 = (op5 <= 0x05 || op5 == 0x14);
        d->dst  = s1_int ? (int8_t)(32 + rd) : (dst_int ? rv_nz(rd) : (int8_t)(32 + rd));
        d->src1 = s1_int ? rv_nz(rs1) : (int8_t)(32 + rs1);
        if (has_rs2) d->src2 = (int8_t)(32 + rs2);
        return;
    }

    default:
        /* Unknown / vendor-specific (Andes Xandes, vector, ...).
           Leave class ALU and mark !ok so the caller falls back to the
           Spike commit log for registers / memory. */
        d->ok = 0;
        d->dst = rv_nz(rd);
        return;
    }
}

static inline void rv_decode(uint32_t insn, rv_dec_t *d)
{
    d->uclass = RC_ALU; d->src1 = d->src2 = d->src3 = d->dst = -1;
    d->is_mem = d->is_store = d->msize_log2 = 0;
    d->is_ctrl = d->is_cond = 0; d->br_type = BT_COND;
    d->push_ras = d->pop_ras = 0;
    d->has_target = 0; d->br_offset = 0;
    d->serialize = 0; d->ok = 0;
    if ((insn & 3) == 3) { d->ilen = 4; rv_decode_32(insn, d); }
    else                 { d->ilen = 2; rv_decode_c(insn & 0xffff, d); }
}
#endif /* RV64_DECODE_H */
