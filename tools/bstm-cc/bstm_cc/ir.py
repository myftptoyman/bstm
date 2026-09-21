"""bit-sliced IR：一張純 bitwise 運算的 DAG。

每個 value 代表「一條 net 的某個 bit，在所有 lane 上的值」，也就是產生出來的
C 裡的一個 word（uint64_t / uint32x4_t / __m512i）。

value 的種類:
  CONST0 / CONST1   常數 0 / 全 1                → 直接寫成 VZERO / VONES
  QREAD(k)          state bit k 的上一 cycle 值  → 直接寫成 cur->q[k]（可重算，不佔 slot）
  PORT(name, bit)   輸入埠                        → 直接寫成引數（可重算，不佔 slot）
  OP(kind, args)    真正要算的東西                → 佔一個 slot

Builder 在建構時就做常數傳播、代數化簡與 CSE（P2）。這讓 backend 產生的程式碼
大幅變少，例如 `(VZERO & ~S) | (B & S)` 會在建 IR 的時候就塌成 `B & S`。
"""

# --- value kind ---
K_CONST = 0
K_QREAD = 1
K_PORT = 2
K_OP = 3

# --- op kind ---
NOT = "NOT"
AND = "AND"
OR = "OR"
XOR = "XOR"
ANDN = "ANDN"    # a & ~b
MUX = "MUX"      # s ? b : a   （參數順序 s, a, b，跟 $mux 的 S/A/B 對齊）
MAJ3 = "MAJ3"    # (a&b) | (c&(a^b))   進位用的多數閘

OP_ARITY = {NOT: 1, AND: 2, OR: 2, XOR: 2, ANDN: 2, MUX: 3, MAJ3: 3}
COMMUTATIVE = (AND, OR, XOR)

CONST0 = 0
CONST1 = 1


class Builder(object):
    def __init__(self):
        # value table；index 0/1 保留給常數
        self.kind = [K_CONST, K_CONST]
        self.info = [0, 1]           # const: 0/1 ; qread: k ; port: (name,bit) ; op: (kind, args...)
        self.ops = []                # 依建立順序的 op value id（= 合法的拓樸順序）
        self._hash = {}              # 結構雜湊 → value（CSE）
        self._qread = {}
        self._port = {}
        self.names = {}              # value → 除錯用的 net 名稱

    # ---------------- 葉節點 ----------------
    def const(self, b):
        return CONST1 if b else CONST0

    def qread(self, k):
        v = self._qread.get(k)
        if v is None:
            v = len(self.kind)
            self.kind.append(K_QREAD)
            self.info.append(k)
            self._qread[k] = v
        return v

    def port(self, name, bit):
        key = (name, bit)
        v = self._port.get(key)
        if v is None:
            v = len(self.kind)
            self.kind.append(K_PORT)
            self.info.append(key)
            self._port[key] = v
        return v

    # ---------------- 查詢 ----------------
    def is_op(self, v):
        return self.kind[v] == K_OP

    def is_const(self, v):
        return v == CONST0 or v == CONST1

    def op_kind(self, v):
        return self.info[v][0] if self.kind[v] == K_OP else None

    def op_args(self, v):
        return self.info[v][1:] if self.kind[v] == K_OP else ()

    def _is_not_of(self, v):
        """v 是不是某個 value 的反相？是的話回傳那個 value，否則 None。"""
        if v == CONST0:
            return CONST1
        if v == CONST1:
            return CONST0
        if self.kind[v] == K_OP and self.info[v][0] == NOT:
            return self.info[v][1]
        return None

    def _inverted(self, a, b):
        """a == ~b ?"""
        return self._is_not_of(a) == b or self._is_not_of(b) == a

    # ---------------- 建 op（含 CSE） ----------------
    def _mk(self, kind, *args):
        key = (kind, args)
        v = self._hash.get(key)
        if v is not None:
            return v
        v = len(self.kind)
        self.kind.append(K_OP)
        self.info.append((kind,) + args)
        self._hash[key] = v
        self.ops.append(v)
        return v

    # ---------------- 化簡過的建構子 ----------------
    def v_not(self, a):
        n = self._is_not_of(a)
        if n is not None:
            return n
        return self._mk(NOT, a)

    def v_and(self, a, b):
        if a == b:
            return a
        if a == CONST0 or b == CONST0:
            return CONST0
        if a == CONST1:
            return b
        if b == CONST1:
            return a
        if self._inverted(a, b):
            return CONST0
        na, nb = self._is_not_of(a), self._is_not_of(b)
        if na is not None and nb is not None:          # ~x & ~y = ~(x|y)
            return self.v_not(self.v_or(na, nb))
        if nb is not None:                             # a & ~y
            return self._mk(ANDN, a, nb)
        if na is not None:                             # ~x & b
            return self._mk(ANDN, b, na)
        return self._mk(AND, *sorted((a, b)))

    def v_or(self, a, b):
        if a == b:
            return a
        if a == CONST1 or b == CONST1:
            return CONST1
        if a == CONST0:
            return b
        if b == CONST0:
            return a
        if self._inverted(a, b):
            return CONST1
        na, nb = self._is_not_of(a), self._is_not_of(b)
        if na is not None and nb is not None:          # ~x | ~y = ~(x&y)
            return self.v_not(self.v_and(na, nb))
        return self._mk(OR, *sorted((a, b)))

    def v_xor(self, a, b):
        if a == b:
            return CONST0
        if a == CONST0:
            return b
        if b == CONST0:
            return a
        if a == CONST1:
            return self.v_not(b)
        if b == CONST1:
            return self.v_not(a)
        if self._inverted(a, b):
            return CONST1
        na, nb = self._is_not_of(a), self._is_not_of(b)
        if na is not None and nb is not None:          # ~x ^ ~y = x ^ y
            return self.v_xor(na, nb)
        if na is not None:                             # ~x ^ y = ~(x^y)
            return self.v_not(self.v_xor(na, b))
        if nb is not None:
            return self.v_not(self.v_xor(a, nb))
        return self._mk(XOR, *sorted((a, b)))

    def v_xnor(self, a, b):
        return self.v_not(self.v_xor(a, b))

    def v_andn(self, a, b):
        """a & ~b"""
        return self.v_and(a, self.v_not(b))

    def v_mux(self, s, a, b):
        """s ? b : a"""
        if s == CONST0:
            return a
        if s == CONST1:
            return b
        if a == b:
            return a
        if a == CONST0:
            return self.v_and(b, s)
        if b == CONST0:
            return self.v_and(a, self.v_not(s))
        if a == CONST1:
            return self.v_or(self.v_not(s), b)
        if b == CONST1:
            return self.v_or(s, a)
        if self._inverted(a, b):
            # a = ~b  →  s ? b : ~b  =  ~(b ^ s)
            return self.v_xnor(b, s)
        ns = self._is_not_of(s)
        if ns is not None:
            return self._mk(MUX, ns, b, a)
        return self._mk(MUX, s, a, b)

    def v_maj3(self, a, b, c):
        """(a&b) | (c & (a^b))  —— 全加器的進位輸出"""
        if a == CONST0:
            return self.v_and(b, c)
        if b == CONST0:
            return self.v_and(a, c)
        if c == CONST0:
            return self.v_and(a, b)
        if a == CONST1:
            return self.v_or(b, c)
        if b == CONST1:
            return self.v_or(a, c)
        if c == CONST1:
            return self.v_or(a, b)
        if a == b:
            return a
        if a == c:
            return a
        if b == c:
            return b
        if self._inverted(a, b):
            return c
        if self._inverted(a, c):
            return b
        if self._inverted(b, c):
            return a
        args = tuple(sorted((a, b, c)))
        return self._mk(MAJ3, *args)

    # ---------------- 多輸入的樹狀化簡 ----------------
    def reduce_tree(self, fn, vals, ident):
        """用平衡樹而不是線性鏈把 vals 收成一個值，縮短相依鏈長度。"""
        vals = [v for v in vals]
        if not vals:
            return ident
        while len(vals) > 1:
            nxt = []
            for i in range(0, len(vals) - 1, 2):
                nxt.append(fn(vals[i], vals[i + 1]))
            if len(vals) & 1:
                nxt.append(vals[-1])
            vals = nxt
        return vals[0]

    def v_or_n(self, vals):
        return self.reduce_tree(self.v_or, vals, CONST0)

    def v_and_n(self, vals):
        return self.reduce_tree(self.v_and, vals, CONST1)

    def v_xor_n(self, vals):
        return self.reduce_tree(self.v_xor, vals, CONST0)

    # ---------------- 多位元輔助 ----------------
    def extend(self, bits, width, signed):
        """把 bit list 補/截到 width 位元。signed 的話補符號位。"""
        bits = list(bits)
        if len(bits) >= width:
            return bits[:width]
        fill = bits[-1] if (signed and bits) else CONST0
        return bits + [fill] * (width - len(bits))

    def add_sub(self, A, B, width, sub, signed):
        """ripple-carry 加減法，回傳 (Y bits, carry_out)。"""
        a = self.extend(A, width, signed)
        b = self.extend(B, width, signed)
        if sub:
            b = [self.v_not(x) for x in b]
        c = CONST1 if sub else CONST0
        Y = []
        for i in range(width):
            t = self.v_xor(a[i], b[i])
            Y.append(self.v_xor(t, c))
            c = self.v_maj3(a[i], b[i], c)
        return Y, c

    def less_than(self, A, B, signed):
        """A < B（signed 或 unsigned），回傳 1 個 bit。

        做法：兩邊都延伸到 W+1 位元（signed 補符號、unsigned 補 0），
        做 W+1 位元減法，結果的 MSB 就是 A<B。
        """
        w = max(len(A), len(B), 1) + 1
        Y, _ = self.add_sub(A, B, w, sub=True, signed=signed)
        return Y[-1]

    def equal(self, A, B, signed):
        w = max(len(A), len(B), 1)
        a = self.extend(A, w, signed)
        b = self.extend(B, w, signed)
        diff = [self.v_xor(a[i], b[i]) for i in range(w)]
        return self.v_not(self.v_or_n(diff))

    def eq_const(self, A, k):
        """A == 常數 k（Python int）。"""
        terms = []
        for i, a in enumerate(A):
            terms.append(a if ((k >> i) & 1) else self.v_not(a))
        if (k >> len(A)) != 0:
            return CONST0
        return self.v_and_n(terms)


class Sinks(object):
    """IR 的終點：state bit 的下一個值、以及輸出埠。"""

    def __init__(self):
        self.ff = {}        # state bit index -> value
        self.out = []       # (port name, bit index, value)
