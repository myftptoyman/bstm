"""cell dispatch table：word-level cell → bit-sliced IR。

每個 handler 拿到 (ctx, name, cell)，用 ctx.A/ctx.B/... 取輸入的 value list，
回傳 Y 的 value list（LSB first）。ctx 會負責把結果綁到 net 上。

這裡只處理「組合」cell。所有 FF / memory 由 compiler.py 處理（它們要配 state bit）。
"""

from .errors import UnsupportedCell, src_of
from .netlist import pint
from . import ir


class LowerCtx(object):
    def __init__(self, builder, bits_of, cell_name, cell):
        self.b = builder
        self._bits_of = bits_of
        self.cell_name = cell_name
        self.cell = cell

    def conn(self, port):
        c = self.cell["connections"]
        if port not in c:
            self.fail("缺少連接埠 %s" % port)
        return self._bits_of(c[port])

    def has(self, port):
        return port in self.cell["connections"]

    def conn_width(self, port):
        """只看寬度，不去讀值（輸出埠不可以用 conn() 讀）。"""
        return len(self.cell["connections"].get(port, ()))

    def param(self, key, default=None):
        return pint(self.cell, key, default)

    def signed(self):
        a = self.param("A_SIGNED", 0)
        b = self.param("B_SIGNED", 0)
        return bool(a and b)

    def fail(self, detail):
        raise UnsupportedCell(self.cell_name, self.cell.get("type"),
                              src_of(self.cell), detail)

    # 常用簡寫
    @property
    def A(self):
        return self.conn("A")

    @property
    def B(self):
        return self.conn("B")

    @property
    def S(self):
        return self.conn("S")

    @property
    def ywidth(self):
        n = self.param("Y_WIDTH", 0)
        if n:
            return n
        return self.conn_width("Y")


# ---------------------------------------------------------------- 位元運算

def _bitwise2(ctx, fn):
    b = ctx.b
    w = ctx.ywidth
    sg = ctx.signed()
    A = b.extend(ctx.A, w, bool(ctx.param("A_SIGNED", 0)) and sg)
    B = b.extend(ctx.B, w, bool(ctx.param("B_SIGNED", 0)) and sg)
    return [fn(A[i], B[i]) for i in range(w)]


def c_and(ctx):
    return _bitwise2(ctx, ctx.b.v_and)


def c_or(ctx):
    return _bitwise2(ctx, ctx.b.v_or)


def c_xor(ctx):
    return _bitwise2(ctx, ctx.b.v_xor)


def c_xnor(ctx):
    return _bitwise2(ctx, ctx.b.v_xnor)


def c_not(ctx):
    b = ctx.b
    w = ctx.ywidth
    A = b.extend(ctx.A, w, bool(ctx.param("A_SIGNED", 0)))
    return [b.v_not(x) for x in A]


def c_pos(ctx):
    b = ctx.b
    return b.extend(ctx.A, ctx.ywidth, bool(ctx.param("A_SIGNED", 0)))


def c_neg(ctx):
    b = ctx.b
    w = ctx.ywidth
    Y, _ = b.add_sub([], ctx.A, w, sub=True, signed=bool(ctx.param("A_SIGNED", 0)))
    return Y


def c_buf(ctx):
    return ctx.b.extend(ctx.A, ctx.ywidth, bool(ctx.param("A_SIGNED", 0)))


# ---------------------------------------------------------------- 歸約

def _pad1(ctx, v):
    """把 1 bit 結果補到 Y_WIDTH（高位補 0）。"""
    w = ctx.ywidth
    return [v] + [ir.CONST0] * (w - 1)


def c_reduce_and(ctx):
    return _pad1(ctx, ctx.b.v_and_n(ctx.A))


def c_reduce_or(ctx):
    return _pad1(ctx, ctx.b.v_or_n(ctx.A))


def c_reduce_bool(ctx):
    return _pad1(ctx, ctx.b.v_or_n(ctx.A))


def c_reduce_xor(ctx):
    return _pad1(ctx, ctx.b.v_xor_n(ctx.A))


def c_reduce_xnor(ctx):
    return _pad1(ctx, ctx.b.v_not(ctx.b.v_xor_n(ctx.A)))


def c_logic_not(ctx):
    return _pad1(ctx, ctx.b.v_not(ctx.b.v_or_n(ctx.A)))


def c_logic_and(ctx):
    b = ctx.b
    return _pad1(ctx, b.v_and(b.v_or_n(ctx.A), b.v_or_n(ctx.B)))


def c_logic_or(ctx):
    b = ctx.b
    return _pad1(ctx, b.v_or(b.v_or_n(ctx.A), b.v_or_n(ctx.B)))


# ---------------------------------------------------------------- 比較

def c_eq(ctx):
    return _pad1(ctx, ctx.b.equal(ctx.A, ctx.B, ctx.signed()))


def c_ne(ctx):
    return _pad1(ctx, ctx.b.v_not(ctx.b.equal(ctx.A, ctx.B, ctx.signed())))


def c_eqx(ctx):
    # 2-state 模型裡 ===/!== 等同 ==/!=
    return c_eq(ctx)


def c_nex(ctx):
    return c_ne(ctx)


def c_lt(ctx):
    return _pad1(ctx, ctx.b.less_than(ctx.A, ctx.B, ctx.signed()))


def c_ge(ctx):
    return _pad1(ctx, ctx.b.v_not(ctx.b.less_than(ctx.A, ctx.B, ctx.signed())))


def c_gt(ctx):
    return _pad1(ctx, ctx.b.less_than(ctx.B, ctx.A, ctx.signed()))


def c_le(ctx):
    return _pad1(ctx, ctx.b.v_not(ctx.b.less_than(ctx.B, ctx.A, ctx.signed())))


# ---------------------------------------------------------------- 算術

def c_add(ctx):
    b = ctx.b
    Y, _ = b.add_sub(ctx.A, ctx.B, ctx.ywidth, sub=False, signed=ctx.signed())
    return Y


def c_sub(ctx):
    b = ctx.b
    Y, _ = b.add_sub(ctx.A, ctx.B, ctx.ywidth, sub=True, signed=ctx.signed())
    return Y


# ---------------------------------------------------------------- 選擇

def c_mux(ctx):
    b = ctx.b
    w = ctx.ywidth
    S = ctx.S
    if len(S) == 1:
        A = b.extend(ctx.A, w, False)
        B = b.extend(ctx.B, w, False)
        s = S[0]
        return [b.v_mux(s, A[i], B[i]) for i in range(w)]
    # 多位元 select。yosys 正規的寫法是 $bmux（A 是 2^|S| 段接起來、沒有 B）；
    # 有些流程會把它直接掛成 $mux，這裡一併吃下來。
    A = ctx.A
    n = 1 << len(S)
    if not ctx.has("B") and len(A) >= n * w:
        return _bmux_bits(b, A, S, w)
    if ctx.has("B") and len(A) + len(ctx.B) >= n * w:
        return _bmux_bits(b, list(A) + list(ctx.B), S, w)
    ctx.fail("$mux 的 S 寬 %d 但 A/B 的寬度湊不出 %d 段 x %d bit" % (len(S), n, w))


def _bmux_bits(b, A, S, w):
    n = 1 << len(S)
    sel = [list(A[k * w:(k + 1) * w]) for k in range(n)]
    for si in range(len(S)):
        s = S[si]
        nxt = []
        for k in range(0, len(sel), 2):
            nxt.append([b.v_mux(s, sel[k][i], sel[k + 1][i]) for i in range(w)])
        sel = nxt
    return sel[0]


def c_bmux(ctx):
    """A 是 (1<<S_WIDTH) 個 WIDTH-bit 的字串接起來，用 S 當二進位索引。"""
    b = ctx.b
    w = ctx.param("WIDTH", ctx.ywidth)
    S = ctx.S
    A = ctx.A
    n = 1 << len(S)
    if len(A) < n * w:
        ctx.fail("$bmux 的 A 寬度 %d 不足 %d" % (len(A), n * w))
    return _bmux_bits(b, A, S, w)


def c_demux(ctx):
    b = ctx.b
    w = ctx.param("WIDTH", 1)
    S = ctx.S
    A = ctx.A
    n = 1 << len(S)
    out = []
    for k in range(n):
        hit = b.eq_const(S, k)
        for i in range(w):
            out.append(b.v_and(A[i] if i < len(A) else ir.CONST0, hit))
    return out


def c_pmux(ctx):
    """$pmux：S 選 B 的第 j 段，全 0 選 A。

    依 yosys simlib 的語意是「後面的 S 蓋掉前面的」（index 大的優先）。
    對 proc_mux/pmuxtree 產生的 one-hot S 來說兩種寫法等價，但這裡照 simlib
    實作以保證跟 Verilator 逐 cycle bit-exact。
    """
    b = ctx.b
    w = ctx.ywidth
    S = ctx.S
    A = b.extend(ctx.A, w, False)
    B = ctx.B
    if len(B) < w * len(S):
        ctx.fail("$pmux 的 B 寬度 %d 不足 %d" % (len(B), w * len(S)))
    out = list(A)
    for j in range(len(S)):
        s = S[j]
        for i in range(w):
            out[i] = b.v_mux(s, out[i], B[j * w + i])
    return out


# ---------------------------------------------------------------- 位移

def _shift(ctx, right, arith):
    """用 log-shifter（barrel）展開。B 是無號位移量（$sshr/$shr 的 B 一律無號）。"""
    b = ctx.b
    w = ctx.ywidth
    a_signed = bool(ctx.param("A_SIGNED", 0))
    A = b.extend(ctx.A, w, a_signed)
    fill = A[-1] if (arith and a_signed and A) else ir.CONST0
    B = ctx.B
    if ctx.param("B_SIGNED", 0):
        ctx.fail("不支援有號的位移量（請在 Verilog 裡把位移量宣告成 unsigned）")
    cur = list(A)
    nbits = len(B)
    # 只有低 ceil(log2(w)) 位元會造成部分位移，其餘只要有 1 就整個移光
    useful = max(1, (w - 1).bit_length())
    for j in range(min(nbits, useful)):
        amt = 1 << j
        s = B[j]
        if right:
            shifted = cur[amt:] + [fill] * amt
        else:
            shifted = [ir.CONST0] * amt + cur[:w - amt]
        cur = [b.v_mux(s, cur[i], shifted[i]) for i in range(w)]
    if nbits > useful:
        over = b.v_or_n(B[useful:])
        cur = [b.v_mux(over, cur[i], fill if right else ir.CONST0) for i in range(w)]
    return cur


def c_shl(ctx):
    return _shift(ctx, right=False, arith=False)


def c_shr(ctx):
    return _shift(ctx, right=True, arith=False)


def c_sshl(ctx):
    return _shift(ctx, right=False, arith=False)


def c_sshr(ctx):
    return _shift(ctx, right=True, arith=True)


COMB = {
    "$and": c_and, "$or": c_or, "$xor": c_xor, "$xnor": c_xnor,
    "$not": c_not, "$pos": c_pos, "$neg": c_neg, "$buf": c_buf,
    "$reduce_and": c_reduce_and, "$reduce_or": c_reduce_or,
    "$reduce_xor": c_reduce_xor, "$reduce_xnor": c_reduce_xnor,
    "$reduce_bool": c_reduce_bool,
    "$logic_not": c_logic_not, "$logic_and": c_logic_and, "$logic_or": c_logic_or,
    "$eq": c_eq, "$ne": c_ne, "$eqx": c_eqx, "$nex": c_nex,
    "$lt": c_lt, "$le": c_le, "$gt": c_gt, "$ge": c_ge,
    "$add": c_add, "$sub": c_sub,
    "$mux": c_mux, "$pmux": c_pmux, "$bmux": c_bmux, "$demux": c_demux,
    "$shl": c_shl, "$shr": c_shr, "$sshl": c_sshl, "$sshr": c_sshr,
}

# 明確說明「為什麼不支援」的 cell，錯誤訊息比較有用
REJECT = {
    "$mul": "乘法違反建模規則 1（欄位 ≤ 8 bit、禁止寬算術）；請改寫成查表或移位加法",
    "$div": "除法違反建模規則 1；請改寫",
    "$mod": "取餘違反建模規則 1；請改寫",
    "$divfloor": "除法違反建模規則 1；請改寫",
    "$modfloor": "取餘違反建模規則 1；請改寫",
    "$pow": "冪次違反建模規則 1；請改寫",
    "$dlatch": "latch 違反建模規則 3（純同步單一 clock domain）；請改成 always @(posedge clk)",
    "$adlatch": "latch 違反建模規則 3；請改成 always @(posedge clk)",
    "$dlatchsr": "latch 違反建模規則 3；請改成 always @(posedge clk)",
    "$sr": "SR latch 違反建模規則 3；請改成同步邏輯",
    "$dffsr": "同時帶非同步 set 與 reset 的 FF 不支援；請改成同步 reset",
    "$dffsre": "同時帶非同步 set 與 reset 的 FF 不支援；請改成同步 reset",
    "$aldff": "load-enable 非同步 FF 不支援；請改成同步邏輯",
    "$aldffe": "load-enable 非同步 FF 不支援；請改成同步邏輯",
    "$specify2": "specify block 不影響功能，請用 read_verilog -nospecify 移除",
    "$specify3": "specify block 不影響功能，請用 read_verilog -nospecify 移除",
    "$specrule": "specify block 不影響功能，請用 read_verilog -nospecify 移除",
    "$assert": "formal cell 不支援，請在 yosys 腳本加 chformal -remove",
    "$assume": "formal cell 不支援，請在 yosys 腳本加 chformal -remove",
    "$cover": "formal cell 不支援，請在 yosys 腳本加 chformal -remove",
    "$live": "formal cell 不支援，請在 yosys 腳本加 chformal -remove",
    "$fair": "formal cell 不支援，請在 yosys 腳本加 chformal -remove",
    "$anyconst": "formal cell 不支援，請在 yosys 腳本加 chformal -remove",
    "$anyseq": "formal cell 不支援，請在 yosys 腳本加 chformal -remove",
    "$print": "違反建模規則 5（禁止 $display）",
    "$check": "違反建模規則 5",
}

FF_TYPES = ("$dff", "$dffe", "$adff", "$adffe", "$sdff", "$sdffe", "$sdffce")
MEM_TYPES = ("$memrd", "$memrd_v2", "$memwr", "$memwr_v2", "$mem", "$mem_v2",
             "$meminit", "$meminit_v2")
