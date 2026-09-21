"""word 型別抽象 + C 程式碼產生。

同一份 IR 對所有 backend 產生「一模一樣的骨架」，差別只在開頭那段 typedef/macro。
c64 的 macro 就是原生運算子，所以產生的機器碼跟手寫 `a & b` 沒有差別。
"""

from . import ir
from .errors import BstmError


class Backend(object):
    name = "c64"
    lanes = 64
    header = "#include <stdint.h>"
    align = 8
    prelude = """typedef uint64_t vec_t;
#define VZERO          ((vec_t)0)
#define VONES          (~(vec_t)0)
#define V_NOT(a)       (~(a))
#define V_AND(a,b)     ((a)&(b))
#define V_OR(a,b)      ((a)|(b))
#define V_XOR(a,b)     ((a)^(b))
#define V_ANDN(a,b)    ((a)&~(b))
#define V_MUX(s,a,b)   ((((a)&~(s))|((b)&(s))))
#define V_MAJ3(a,b,c)  ((((a)&(b))|((c)&((a)^(b)))))
#define V_LANE(a,i)    ((unsigned)(((a)>>(i))&1))
#define V_BCAST(x)     ((x)?VONES:VZERO)
"""


class C64(Backend):
    pass


class Neon(Backend):
    name = "neon"
    lanes = 128
    align = 16
    header = "#include <stdint.h>\n#include <arm_neon.h>"
    prelude = """typedef uint32x4_t vec_t;
#define VZERO          vdupq_n_u32(0)
#define VONES          vdupq_n_u32(0xffffffffu)
#define V_NOT(a)       vmvnq_u32(a)
#define V_AND(a,b)     vandq_u32((a),(b))
#define V_OR(a,b)      vorrq_u32((a),(b))
#define V_XOR(a,b)     veorq_u32((a),(b))
#define V_ANDN(a,b)    vbicq_u32((a),(b))                 /* a & ~b */
#define V_MUX(s,a,b)   vbslq_u32((s),(b),(a))             /* s ? b : a，一條 BSL */
#define V_MAJ3(a,b,c)  vbslq_u32(veorq_u32((a),(b)),(c),vandq_u32((a),(b)))
#define V_LANE(a,i)    bstm_lane128((a),(i))
#define V_BCAST(x)     ((x)?VONES:VZERO)
static inline unsigned bstm_lane128(uint32x4_t a, int i) {
    uint32_t tmp[4];
    vst1q_u32(tmp, a);                       /* lane index 可以是執行期變數 */
    return (tmp[i >> 5] >> (i & 31)) & 1u;
}
"""


class Avx512(Backend):
    name = "avx512"
    lanes = 512
    align = 64
    header = "#include <stdint.h>\n#include <immintrin.h>"
    prelude = """typedef __m512i vec_t;
#define VZERO          _mm512_setzero_si512()
#define VONES          _mm512_set1_epi32(-1)
#define V_NOT(a)       _mm512_ternarylogic_epi32((a),(a),(a),0x55)
#define V_AND(a,b)     _mm512_and_si512((a),(b))
#define V_OR(a,b)      _mm512_or_si512((a),(b))
#define V_XOR(a,b)     _mm512_xor_si512((a),(b))
#define V_ANDN(a,b)    _mm512_andnot_si512((b),(a))       /* a & ~b */
#define V_MUX(s,a,b)   _mm512_ternarylogic_epi32((s),(a),(b),0xac)  /* s ? b : a */
#define V_MAJ3(a,b,c)  _mm512_ternarylogic_epi32((a),(b),(c),0xe8)
#define V_LANE(a,i)    bstm_lane512((a),(i))
#define V_BCAST(x)     ((x)?VONES:VZERO)
static inline unsigned bstm_lane512(__m512i a, int i) {
    uint32_t tmp[16];
    _mm512_storeu_si512((void*)tmp, a);
    return (tmp[i >> 5] >> (i & 31)) & 1u;
}
"""


BACKENDS = {"c64": C64, "neon": Neon, "avx512": Avx512}

OPFMT = {
    ir.NOT: "V_NOT(%s)",
    ir.AND: "V_AND(%s, %s)",
    ir.OR: "V_OR(%s, %s)",
    ir.XOR: "V_XOR(%s, %s)",
    ir.ANDN: "V_ANDN(%s, %s)",
    ir.MUX: "V_MUX(%s, %s, %s)",
    ir.MAJ3: "V_MAJ3(%s, %s, %s)",
}


RESERVED = set("""
auto break case char const continue default do double else enum extern float for
goto if inline int long register restrict return short signed sizeof static struct
switch typedef union unsigned void volatile while
vec_t state_t next cur bstm_s eval_cycle bstm_init_state bstm_describe
VZERO VONES V_NOT V_AND V_OR V_XOR V_ANDN V_MUX V_MAJ3 V_LANE V_BCAST
BSTM_LANES BSTM_NSTATE BSTM_NSLOTS
""".split())


def c_ident(name):
    return "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in name)


def check_names(ports):
    for p in ports:
        n = c_ident(p.name)
        if n in RESERVED:
            raise BstmError(
                "埠名 %r 跟產生出來的 C 保留字/內部符號撞名，請改名" % p.name)


class Emitter(object):
    def __init__(self, design, backend, lanes=None, comments=True, chunk=1000):
        self.d = design
        self.be = backend
        self.comments = comments
        self.chunk = chunk
        if lanes is not None and lanes != backend.lanes:
            raise BstmError("backend %s 的 lane 數固定是 %d，不能設成 %d"
                            % (backend.name, backend.lanes, lanes))
        self.lanes = backend.lanes
        self.out = []

    def P(self, s=""):
        self.out.append(s)

    # ------------------------------------------------ value → C 表示式
    def ref(self, v):
        b = self.d.builder
        k = b.kind[v]
        if k == ir.K_CONST:
            return "VONES" if v == ir.CONST1 else "VZERO"
        if k == ir.K_QREAD:
            return "cur->q[%d]" % b.info[v]
        if k == ir.K_PORT:
            name, bit = b.info[v]
            p = self.portmap[name]
            return name if p.width == 1 else "%s[%d]" % (name, bit)
        return "bstm_s[%d]" % self.d.slot_of[v]

    # ------------------------------------------------ 介面
    def in_params(self):
        out = []
        for p in self.d.inputs:
            if p.width == 1:
                out.append("vec_t %s" % c_ident(p.name))
            else:
                out.append("const vec_t %s[%d]" % (c_ident(p.name), p.width))
        return out

    def in_args(self):
        return [c_ident(p.name) for p in self.d.inputs]

    def signature(self):
        args = ["state_t *restrict next", "const state_t *restrict cur"]
        args += self.in_params()
        for p in self.d.outputs:
            if p.width == 1:
                args.append("vec_t *%s" % c_ident(p.name))
            else:
                args.append("vec_t %s[%d]" % (c_ident(p.name), p.width))
        return ("void eval_cycle(" +
                (",\n                       ".join(args)) + ")")

    def chunk_signature(self, k):
        args = ["vec_t *restrict bstm_s", "state_t *restrict next",
                "const state_t *restrict cur"] + self.in_params()
        return ("static void bstm_chunk_%d(" % k +
                (",\n        ".join(args)) + ")")

    def emit(self):
        d = self.d
        b = d.builder
        check_names(d.inputs + d.outputs)
        self.portmap = {}
        for p in d.inputs + d.outputs:
            self.portmap[p.name] = p
        P = self.P
        P("/* 自動產生，請勿手改 —— bstm-cc */")
        P("/* 模組: %s   backend: %s   lanes: %d */"
          % (d.name, self.be.name, self.lanes))
        P("/* nets=%d  cells=%d  state=%d  ops=%d  slots=%d */"
          % (d.stats["nets"], d.stats["cells"], d.stats["state_bits"],
             d.stats["ops_live"], d.stats["slots"]))
        P("")
        P(self.be.header)
        P("#include <string.h>")
        P("")
        P("#define BSTM_LANES %d" % self.lanes)
        P("#define BSTM_NSTATE %d" % len(d.state))
        P("#define BSTM_NSTATE_ARR %d" % max(1, len(d.state)))
        P("#define BSTM_NSLOTS %d" % d.nslots)
        P("")
        P(self.be.prelude)
        P("")
        self.emit_state()
        self.emit_init()
        P("")
        chunks = self.split_stream()
        if len(chunks) > 1:
            P("/* 產生的程式碼切成 %d 段 static 函式：gcc 的最佳化在單一超大"
              % len(chunks))
            P(" * basic block 上是超線性的，切開之後編譯時間才會回到線性。 */")
            for k, part in enumerate(chunks):
                P(self.chunk_signature(k))
                P("{")
                self.emit_instrs(part)
                P("}")
                P("")
        P(self.signature())
        P("{")
        if d.nslots:
            P("    vec_t bstm_s[%d];" % d.nslots)
        if len(chunks) > 1:
            call = ", ".join(["bstm_s", "next", "cur"] + self.in_args())
            for k in range(len(chunks)):
                P("    bstm_chunk_%d(%s);" % (k, call))
            self.emit_instrs(self.out_instrs())
        else:
            self.emit_instrs(chunks[0] if chunks else [])
            self.emit_instrs(self.out_instrs())
        P("}")
        P("")
        self.emit_desc()
        return "\n".join(self.out) + "\n"

    def emit_state(self):
        d = self.d
        P = self.P
        P("/* state_t：每個 bit 一個 word，%d 個 lane 同時算 */" % self.lanes)
        P("typedef struct {")
        P("    vec_t q[BSTM_NSTATE_ARR];")
        P("} __attribute__((aligned(%d))) state_t;" % self.be.align)
        P("")
        if self.comments:
            P("/* state bit 對照表:")
            for sb in d.state:
                P("     q[%-5d] %-6s %s" % (sb.idx, sb.kind, sb.label))
            P(" */")

    def emit_init(self):
        d = self.d
        P = self.P
        ones = [sb.idx for sb in d.state if sb.init]
        P("")
        P("/* reset 之外的初始值（memory 的 $meminit）。next != cur 是呼叫端的責任。 */")
        P("void bstm_init_state(state_t *st)")
        P("{")
        P("    memset(st, 0, sizeof(*st));")
        for k in ones:
            P("    st->q[%d] = VONES;" % k)
        P("}")

    def split_stream(self):
        """把 op/ff 指令切成每段至多 chunk 個 op 的區塊（out 指令另外處理）。"""
        body = [i for i in self.d.stream if i.op != "out"]
        if not self.chunk or len(body) <= self.chunk:
            return [body]
        parts, cur, n = [], [], 0
        for ins in body:
            cur.append(ins)
            if ins.op == "op":
                n += 1
                if n >= self.chunk:
                    parts.append(cur)
                    cur, n = [], 0
        if cur:
            parts.append(cur)
        return parts

    def out_instrs(self):
        return [i for i in self.d.stream if i.op == "out"]

    def emit_instrs(self, instrs):
        d = self.d
        b = d.builder
        P = self.P
        for ins in instrs:
            if ins.op == "op":
                kind = b.op_kind(ins.value)
                args = tuple(self.ref(a) for a in b.op_args(ins.value))
                expr = OPFMT[kind] % args
                cmt = ""
                if self.comments:
                    nm = b.names.get(ins.value)
                    if nm:
                        cmt = "   /* %s */" % nm
                P("    bstm_s[%d] = %s;%s" % (ins.slot, expr, cmt))
            elif ins.op == "ff":
                sb = d.state[ins.target]
                cmt = "   /* %s */" % sb.label if self.comments else ""
                P("    next->q[%d] = %s;%s" % (ins.target, self.ref(ins.value), cmt))
            else:
                name, bit = ins.target
                p = self.portmap[name]
                lhs = ("*%s" % c_ident(name)) if p.width == 1 else \
                      ("%s[%d]" % (c_ident(name), bit))
                P("    %s = %s;" % (lhs, self.ref(ins.value)))

    def emit_desc(self):
        """PLAN §13.2 ABI 的簡化版：讓 runtime 不用知道模型內部就能掛載。"""
        d = self.d
        P = self.P
        P("/* ---- 簡化版 runtime ABI（PLAN §13.2） ---- */")
        P("typedef struct {")
        P("    const char *name;")
        P("    unsigned    lanes;")
        P("    unsigned    state_bytes;")
        P("    unsigned    n_state_bits;")
        P("    unsigned    n_inputs;")
        P("    unsigned    n_outputs;")
        P("    const char *const *input_names;")
        P("    const unsigned char *input_widths;")
        P("    const char *const *output_names;")
        P("    const unsigned char *output_widths;")
        P("} bstm_model_desc_t;")
        P("")
        P("static const char *const bstm_in_names[] = {%s};"
          % (", ".join('"%s"' % p.name for p in d.inputs) or "0"))
        P("static const unsigned char bstm_in_widths[] = {%s};"
          % (", ".join(str(p.width) for p in d.inputs) or "0"))
        P("static const char *const bstm_out_names[] = {%s};"
          % (", ".join('"%s"' % p.name for p in d.outputs) or "0"))
        P("static const unsigned char bstm_out_widths[] = {%s};"
          % (", ".join(str(p.width) for p in d.outputs) or "0"))
        P("")
        P("const bstm_model_desc_t *bstm_describe(void)")
        P("{")
        P("    static const bstm_model_desc_t d = {")
        P('        "%s", BSTM_LANES, (unsigned)sizeof(state_t), BSTM_NSTATE,' % d.name)
        P("        %d, %d, bstm_in_names, bstm_in_widths,"
          % (len(d.inputs), len(d.outputs)))
        P("        bstm_out_names, bstm_out_widths };")
        P("    return &d;")
        P("}")


def make(name):
    if name not in BACKENDS:
        raise BstmError("不認得的 backend %r（可用：%s）"
                        % (name, ", ".join(sorted(BACKENDS))))
    return BACKENDS[name]()
