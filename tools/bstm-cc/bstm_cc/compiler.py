"""bstm-cc 的主體：網表 → 排程 → IR → DCE → liveness → slot 配置。"""

import heapq

from . import cells as cellmod
from . import ir
from .errors import BstmError, CombLoop, UnsupportedCell, src_of
from .netlist import pbits, pint

# cell 的輸出埠（當 JSON 沒帶 port_directions 時的後備表）
OUTPUT_PORTS = {
    "$memrd": ("DATA",), "$memrd_v2": ("DATA",),
    "$mem": ("RD_DATA",), "$mem_v2": ("RD_DATA",),
    "$demux": ("Y",),
}
DEFAULT_OUT = ("Y", "Q")

CLOCKY = ("CLK", "RD_CLK", "WR_CLK")


class StateBit(object):
    """state_t 裡的一個 word。"""
    __slots__ = ("idx", "kind", "label", "init")

    def __init__(self, idx, kind, label, init=0):
        self.idx = idx
        self.kind = kind          # 'ff' | 'mem' | 'rdreg'
        self.label = label
        self.init = init


class Instr(object):
    __slots__ = ("op", "value", "target", "slot")

    def __init__(self, op, value, target=None):
        self.op = op              # 'op' | 'ff' | 'out'
        self.value = value
        self.target = target
        self.slot = None


class Design(object):
    """編譯結果：backend 需要的一切。"""

    def __init__(self):
        self.name = ""
        self.builder = None
        self.state = []           # list[StateBit]
        self.stream = []          # list[Instr]
        self.nslots = 0
        self.inputs = []          # list[Port]
        self.outputs = []         # list[Port]
        self.slot_of = {}         # value -> slot
        self.stats = {}
        self.warnings = []


# ------------------------------------------------------------------ 主流程

class Compiler(object):
    def __init__(self, module, verbose=False, scheduler="auto"):
        self.scheduler = scheduler
        self.m = module
        self.b = ir.Builder()
        self.verbose = verbose
        self.warnings = []
        self.state = []
        self.netval = {}
        self.ff_next = {}         # state idx -> value
        self.clock_nets = set()
        self.debug_names = module.net_debug_names()

    # -------------------------------------------------- 小工具
    def warn(self, msg):
        self.warnings.append(msg)

    def bits_of(self, conn):
        out = []
        for bb in conn:
            if isinstance(bb, int):
                v = self.netval.get(bb)
                if v is None:
                    self.warn("net %d 沒有驅動源，當成 0 處理" % bb)
                    v = ir.CONST0
                    self.netval[bb] = v
                out.append(v)
            else:
                out.append(ir.CONST1 if bb == "1" else ir.CONST0)
        return out

    def bind(self, conn, vals):
        for i, bb in enumerate(conn):
            if isinstance(bb, int) and i < len(vals):
                self.netval[bb] = vals[i]
                nm = self.debug_names.get(bb)
                if nm and vals[i] not in self.b.names:
                    self.b.names[vals[i]] = nm

    def add_state(self, kind, label, init=0):
        sb = StateBit(len(self.state), kind, label, init)
        self.state.append(sb)
        return sb.idx

    # -------------------------------------------------- 分類 cell
    def classify(self):
        comb, ffs, mems = [], [], []
        for n, c in self.m.cells.items():
            t = c["type"]
            for cp in CLOCKY:
                for bb in c["connections"].get(cp, []):
                    if isinstance(bb, int):
                        self.clock_nets.add(bb)
            if t in cellmod.FF_TYPES:
                ffs.append((n, c))
            elif t in cellmod.MEM_TYPES:
                mems.append((n, c))
            elif t in cellmod.COMB:
                comb.append((n, c))
            elif t in cellmod.REJECT:
                raise UnsupportedCell(n, t, src_of(c), cellmod.REJECT[t])
            elif not t.startswith("$"):
                raise UnsupportedCell(
                    n, t, src_of(c),
                    "這是一個使用者 module 或 library cell。bstm-cc 只吃 flatten 過的 "
                    "word-level 網表，請在 yosys 腳本裡加 `flatten`（且不要跑 techmap/abc）")
            else:
                raise UnsupportedCell(n, t, src_of(c),
                                      "dispatch table 沒有這個 cell；請回報或改寫 RTL")
        return comb, ffs, mems

    # -------------------------------------------------- state bit 配置
    def alloc_state(self, ffs, mems):
        # (1) FF：依 net id 排序，跟原型的 state_t 佈局一致
        ffbits = []
        for n, c in ffs:
            pol = pint(c, "CLK_POLARITY", 1)
            if not pol:
                raise UnsupportedCell(n, c["type"], src_of(c),
                                      "negedge clock 不支援（建模規則 3：純同步單一 clock domain）")
            for bb in c["connections"].get("CLK", []):
                if isinstance(bb, int):
                    self.clock_nets.add(bb)
            for bb in c["connections"]["Q"]:
                if isinstance(bb, int):
                    ffbits.append(bb)
        dup = len(ffbits) - len(set(ffbits))
        if dup:
            raise BstmError("有 %d 個 net 被多個 FF 同時驅動，網表有問題" % dup)
        self.ffidx = {}
        for bb in sorted(set(ffbits)):
            k = self.add_state("ff", self.debug_names.get(bb, "n%d" % bb))
            self.ffidx[bb] = k
            self.netval[bb] = self.b.qread(k)

        # (2) 記憶體
        self.mem = {}
        self.memops = self.collect_mem_ports(mems)
        for mid, info in sorted(self.mem.items()):
            size, width, off = info["size"], info["width"], info["offset"]
            init = info["init"]
            grid = []
            for e in range(size):
                row = []
                for bidx in range(width):
                    iv = init.get((e, bidx), 0)
                    row.append(self.add_state("mem", "%s[%d][%d]" % (mid, e + off, bidx), iv))
                grid.append(row)
            info["bits"] = grid

    def collect_mem_ports(self, mems):
        """把 $memrd/$memwr/$mem 統一成 (讀埠, 寫埠) 描述。"""
        rds, wrs = [], []
        for n, c in mems:
            t = c["type"]
            if t in ("$meminit", "$meminit_v2"):
                self.take_meminit(n, c)
                continue
            if t in ("$memrd", "$memrd_v2"):
                mid = self.memid(c)
                self.touch_mem(n, c, mid, pint(c, "WIDTH"), pint(c, "ABITS"))
                rds.append((n, c, mid))
            elif t in ("$memwr", "$memwr_v2"):
                mid = self.memid(c)
                self.touch_mem(n, c, mid, pint(c, "WIDTH"), pint(c, "ABITS"))
                wrs.append((n, c, mid, pint(c, "PORTID", 0)))
            elif t in ("$mem", "$mem_v2"):
                self.split_mem(n, c, rds, wrs)
            else:
                raise UnsupportedCell(n, t, src_of(c), "記憶體 cell 型別不支援")
        wrs.sort(key=lambda x: x[3])
        return rds, wrs

    def memid(self, c):
        p = c.get("parameters") or {}
        v = p.get("MEMID")
        if v is None:
            raise BstmError("memory cell 缺少 MEMID")
        return str(v).lstrip("\\")

    def touch_mem(self, n, c, mid, width, abits):
        info = self.mem.get(mid)
        if info is None:
            decl = self.m.memories.get("\\" + mid) or self.m.memories.get(mid) or {}
            size = decl.get("size")
            off = decl.get("start_offset", 0)
            if size is None:
                size = 1 << abits
                if size > 4096:
                    raise UnsupportedCell(
                        n, c["type"], src_of(c),
                        "找不到 memory %s 的宣告、且 ABITS=%d 推出來的大小 %d 太大。"
                        "請讓 yosys 保留 memories 區段（不要跑 memory_collect 之外的 memory_map）"
                        % (mid, abits, size))
            self.mem[mid] = info = {"size": size, "width": decl.get("width", width),
                                    "offset": off, "init": {}, "bits": None}
        return info

    def take_meminit(self, n, c):
        mid = self.memid(c)
        info = self.touch_mem(n, c, mid, pint(c, "WIDTH"), pint(c, "ABITS", 32))
        words = pint(c, "WORDS", 0)
        data = (c.get("parameters") or {}).get("DATA")
        addr = c["connections"].get("ADDR", [])
        if not all(isinstance(x, str) for x in addr):
            raise UnsupportedCell(n, c["type"], src_of(c), "$meminit 的 ADDR 必須是常數")
        base = int("".join("1" if x == "1" else "0" for x in reversed(addr)) or "0", 2)
        if data is None:
            return
        s = str(data)
        w = info["width"]
        flat = [1 if ch == "1" else 0 for ch in reversed(s)]
        for wi in range(words):
            for bi in range(w):
                k = wi * w + bi
                if k < len(flat) and flat[k]:
                    info["init"][(base + wi - info["offset"], bi)] = 1

    def split_mem(self, n, c, rds, wrs):
        """把 memory_collect 之後的 $mem/$mem_v2 拆回單一讀/寫埠。"""
        mid = self.memid(c)
        width = pint(c, "WIDTH")
        abits = pint(c, "ABITS")
        size = pint(c, "SIZE")
        off = pint(c, "OFFSET", 0)
        self.mem.setdefault(mid, {"size": size, "width": width, "offset": off,
                                  "init": {}, "bits": None})
        init = (c.get("parameters") or {}).get("INIT")
        if init is not None and set(str(init)) - set("x"):
            s = str(init)
            flat = [1 if ch == "1" else 0 for ch in reversed(s)]
            for e in range(size):
                for bi in range(width):
                    k = e * width + bi
                    if k < len(flat) and flat[k]:
                        self.mem[mid]["init"][(e, bi)] = 1
        conn = c["connections"]
        nrd = pint(c, "RD_PORTS", 0)
        nwr = pint(c, "WR_PORTS", 0)

        def sub(name, k, w):
            v = conn.get(name, [])
            return list(v[k * w:(k + 1) * w])

        def parm_bits(key, count):
            return pbits(c, key, count)

        rd_ce = parm_bits("RD_CLK_ENABLE", max(nrd, 1))
        wr_ce = parm_bits("WR_CLK_ENABLE", max(nwr, 1))
        trans = parm_bits("RD_TRANSPARENCY_MASK", max(nrd * nwr, 1))
        for k in range(nrd):
            fake = {"type": "$memrd_v2",
                    "parameters": {"MEMID": mid, "ABITS": abits, "WIDTH": width,
                                   "CLK_ENABLE": rd_ce[k], "CLK_POLARITY": 1,
                                   "TRANSPARENCY_MASK":
                                       "".join(str(trans[k * nwr + j]) for j in range(nwr - 1, -1, -1)) or "0"},
                    "attributes": c.get("attributes", {}),
                    "connections": {"CLK": sub("RD_CLK", k, 1),
                                    "EN": sub("RD_EN", k, 1),
                                    "ADDR": sub("RD_ADDR", k, abits),
                                    "DATA": sub("RD_DATA", k, width)}}
            rds.append(("%s.rd%d" % (n, k), fake, mid))
        for k in range(nwr):
            fake = {"type": "$memwr_v2",
                    "parameters": {"MEMID": mid, "ABITS": abits, "WIDTH": width,
                                   "CLK_ENABLE": wr_ce[k], "CLK_POLARITY": 1,
                                   "PORTID": k},
                    "attributes": c.get("attributes", {}),
                    "connections": {"CLK": sub("WR_CLK", k, 1),
                                    "EN": sub("WR_EN", k, width),
                                    "ADDR": sub("WR_ADDR", k, abits),
                                    "DATA": sub("WR_DATA", k, width)}}
            wrs.append(("%s.wr%d" % (n, k), fake, mid, k))

    # -------------------------------------------------- 輸入埠
    def bind_inputs(self):
        self.in_ports = []
        for p in self.m.inputs():
            if p.width == 1 and all(isinstance(bb, int) and bb in self.clock_nets
                                    for bb in p.bits):
                continue                      # clock：cycle-based 模型不需要
            self.in_ports.append(p)
            for i, bb in enumerate(p.bits):
                if isinstance(bb, int):
                    self.netval[bb] = self.b.port(p.name, i)

    # -------------------------------------------------- 讀埠輸出暫存器
    def alloc_rdregs(self):
        rds, _ = self.memops
        self.rdreg = {}
        for n, c, mid in rds:
            if not pint(c, "CLK_ENABLE", 1):
                raise UnsupportedCell(
                    n, c["type"], src_of(c),
                    "非同步讀取的記憶體不支援（建模規則 3）。請把讀取改成同步：\n"
                    "                  always @(posedge clk) rdata <= mem[addr];\n"
                    "                  用一拍讀取延遲，反正真硬體也是這樣")
            if not pint(c, "CLK_POLARITY", 1):
                raise UnsupportedCell(n, c["type"], src_of(c), "negedge 讀取時脈不支援")
            tm = pint(c, "TRANSPARENCY_MASK", 0) or pint(c, "TRANSPARENT", 0)
            if tm:
                raise UnsupportedCell(
                    n, c["type"], src_of(c),
                    "transparent（write-first）讀埠不支援；請改成 read-first，"
                    "或在 RTL 裡自己做 bypass")
            w = pint(c, "WIDTH")
            label = n.split("$")[-1] or n
            idxs = []
            for bidx in range(w):
                idxs.append(self.add_state(
                    "rdreg", "%s.rd<%s>[%d]" % (mid, label, bidx)))
            self.rdreg[n] = idxs
            data = c["connections"]["DATA"]
            self.bind(data, [self.b.qread(k) for k in idxs])

    # -------------------------------------------------- 排程（拓樸 + 就近）
    def schedule(self, comb):
        cellmap = {}
        driver = {}
        for n, c in comb:
            cellmap[n] = c
            for port in self.out_ports_of(c):
                for bb in c["connections"].get(port, []):
                    if isinstance(bb, int):
                        if bb in driver:
                            raise BstmError("net %d 被 %s 和 %s 同時驅動"
                                            % (bb, driver[bb], n))
                        driver[bb] = n
        deps = {}
        users = {}
        for n, c in comb:
            need = set()
            outs = set(self.out_ports_of(c))
            for port, conn in c["connections"].items():
                if port in outs or port in CLOCKY:
                    continue
                for bb in conn:
                    if isinstance(bb, int) and bb in driver and driver[bb] != n:
                        need.add(driver[bb])
            deps[n] = need
            for d in need:
                users.setdefault(d, []).append(n)
        remaining = {n: len(deps[n]) for n, _ in comb}
        order = []
        # LIFO：剛算出來的值馬上被用掉，生命期短 → slot 少
        stack = [n for n, _ in comb if remaining[n] == 0]
        stack.reverse()
        while stack:
            n = stack.pop()
            if remaining[n] < 0:
                continue
            remaining[n] = -1
            order.append(n)
            for u in users.get(n, ()):
                remaining[u] -= 1
                if remaining[u] == 0:
                    stack.append(u)
        if len(order) != len(comb):
            stuck = [n for n, r in remaining.items() if r >= 0]
            names = ", ".join(stuck[:5])
            raise CombLoop("偵測到組合迴圈：還有 %d 個 cell 排不進去（例如 %s）"
                           % (len(stuck), names))
        return [(n, cellmap[n]) for n in order]

    def out_ports_of(self, c):
        pd = c.get("port_directions")
        if pd:
            return tuple(k for k, v in pd.items() if v == "output")
        return OUTPUT_PORTS.get(c["type"], DEFAULT_OUT)

    # -------------------------------------------------- 組合邏輯展開
    def lower_comb(self, order):
        for n, c in order:
            ctx = cellmod.LowerCtx(self.b, self.bits_of, n, c)
            fn = cellmod.COMB[c["type"]]
            Y = fn(ctx)
            self.bind(c["connections"].get("Y", []), Y)

    # -------------------------------------------------- FF
    def lower_ffs(self, ffs):
        b = self.b
        for n, c in ffs:
            t = c["type"]
            conn = c["connections"]
            D = self.bits_of(conn["D"])
            Q = conn["Q"]
            w = len(Q)
            if t in ("$dffe", "$adffe", "$sdffe", "$sdffce"):
                en = self.pol_signal(c, "EN", "EN_POLARITY")
            else:
                en = ir.CONST1
            srst = arst = None
            if t in ("$sdff", "$sdffe", "$sdffce"):
                srst = self.pol_signal(c, "SRST", "SRST_POLARITY")
                rval = pbits(c, "SRST_VALUE", w)
            if t in ("$adff", "$adffe"):
                arst = self.pol_signal(c, "ARST", "ARST_POLARITY")
                rval = pbits(c, "ARST_VALUE", w)
                self.warn("%s 是非同步 reset（%s）。cycle-based 模型把它當成同步 reset 處理，"
                          "跟 Verilator 對拍時 reset 的那一拍可能不一致（建模規則 3 建議改同步）"
                          % (n, t))
            for i in range(w):
                bb = Q[i]
                if not isinstance(bb, int):
                    continue
                k = self.ffidx[bb]
                q = b.qread(k)
                d = D[i] if i < len(D) else ir.CONST0
                if t == "$sdffce":
                    # enable 優先：EN ? (SRST ? RVAL : D) : Q
                    inner = b.v_mux(srst, d, b.const(rval[i]))
                    nxt = b.v_mux(en, q, inner)
                else:
                    nxt = b.v_mux(en, q, d)
                    if srst is not None:
                        nxt = b.v_mux(srst, nxt, b.const(rval[i]))
                    if arst is not None:
                        nxt = b.v_mux(arst, nxt, b.const(rval[i]))
                self.ff_next[k] = nxt

    def pol_signal(self, c, port, polkey):
        sig = self.bits_of(c["connections"][port])[0]
        if not pint(c, polkey, 1):
            sig = self.b.v_not(sig)
        return sig

    # -------------------------------------------------- 記憶體
    def lower_mem(self):
        b = self.b
        rds, wrs = self.memops
        # 讀（用寫入前的內容 → read-first）
        for n, c, mid in rds:
            info = self.mem[mid]
            grid = info["bits"]
            addr = self.bits_of(c["connections"]["ADDR"])
            en = self.bits_of(c["connections"]["EN"])[0]
            w = info["width"]
            data = []
            hits = [b.eq_const(addr, e + info["offset"]) for e in range(info["size"])]
            for bidx in range(w):
                terms = [b.v_and(b.qread(grid[e][bidx]), hits[e])
                         for e in range(info["size"])]
                data.append(b.v_or_n(terms))
            for bidx, k in enumerate(self.rdreg[n]):
                self.ff_next[k] = b.v_mux(en, b.qread(k), data[bidx])
        # 寫（PORTID 小的先、大的蓋掉）
        nxt = {}
        for mid, info in self.mem.items():
            grid = info["bits"]
            for e in range(info["size"]):
                for bidx in range(info["width"]):
                    nxt[grid[e][bidx]] = b.qread(grid[e][bidx])
        for n, c, mid, pid in wrs:
            if not pint(c, "CLK_ENABLE", 1):
                raise UnsupportedCell(n, c["type"], src_of(c),
                                      "非同步寫入不支援（建模規則 3）；請改成 always @(posedge clk)")
            if not pint(c, "CLK_POLARITY", 1):
                raise UnsupportedCell(n, c["type"], src_of(c), "negedge 寫入時脈不支援")
            info = self.mem[mid]
            grid = info["bits"]
            addr = self.bits_of(c["connections"]["ADDR"])
            en = self.bits_of(c["connections"]["EN"])
            data = self.bits_of(c["connections"]["DATA"])
            w = info["width"]
            for e in range(info["size"]):
                hit = b.eq_const(addr, e + info["offset"])
                if hit == ir.CONST0:
                    continue
                for bidx in range(w):
                    k = grid[e][bidx]
                    we = b.v_and(en[bidx] if bidx < len(en) else ir.CONST0, hit)
                    nxt[k] = b.v_mux(we, nxt[k], data[bidx])
        for k, v in nxt.items():
            self.ff_next[k] = v

    # -------------------------------------------------- 出口
    def collect_sinks(self):
        outs = []
        self.out_ports = self.m.outputs()
        for p in self.out_ports:
            for i, bb in enumerate(p.bits):
                if isinstance(bb, int):
                    v = self.netval.get(bb)
                    if v is None:
                        self.warn("輸出埠 %s[%d] 沒有驅動源，當成 0" % (p.name, i))
                        v = ir.CONST0
                else:
                    v = ir.CONST1 if bb == "1" else ir.CONST0
                outs.append((p.name, i, v))
        return outs

    # -------------------------------------------------- 主入口
    def run(self):
        comb, ffs, mems = self.classify()
        self.alloc_state(ffs, mems)
        self.bind_inputs()
        self.alloc_rdregs()
        order = self.schedule(comb)
        self.lower_comb(order)
        self.lower_ffs(ffs)
        if self.mem:
            self.lower_mem()
        outs = self.collect_sinks()
        for k in range(len(self.state)):
            if k not in self.ff_next:
                self.ff_next[k] = self.b.qread(k)

        d = Design()
        d.name = self.m.name
        d.builder = self.b
        d.state = self.state
        d.inputs = self.in_ports
        d.outputs = self.out_ports
        d.warnings = self.warnings
        d.cells = len(self.m.cells)
        d.nets = len(self.m.all_net_bits())

        stream, nslots, slot_of, sname = build_stream(
            self.b, self.ff_next, outs, scheduler=self.scheduler)
        d.stream = stream
        d.nslots = nslots
        d.slot_of = slot_of
        d.ff_next = self.ff_next
        d.outs = outs
        d.stats = {
            "cells": d.cells,
            "nets": d.nets,
            "state_bits": len(self.state),
            "ops_built": len(self.b.ops),
            "ops_live": sum(1 for i in stream if i.op == "op"),
            "slots": nslots,
            "scheduler": sname,
        }
        return d


# ------------------------------------------------------------------ DCE + 排程 + liveness

SCHEDULERS = ("greedy", "dfs")


def build_stream(b, ff_next, outs, scheduler="auto"):
    """DCE → 壓低暫存器壓力的排程 → liveness → 線性掃描配 slot。

    這是 PLAN §6.4(a) 的本體。原型的做法是「每個 net 一個區域變數、FF 更新全擠在
    最後」，於是整個設計的中間值同時活著。這裡做四件事：

      1. DCE：只留 sink（state 更新、輸出埠）真正用得到的 op。
      2. 排程：兩種啟發式各跑一次，取 slot 數比較少的那個。
         - greedy：暫存器壓力導向的 list scheduling，
           代價 = (新增幾個活值) - (殺掉幾個活值)。
         - dfs：從 sink 往回做 DFS post-order，值算出來馬上被消費掉。
         兩者都把 state 更新當成可排程的節點，所以 D 值一算好就寫進 state，
         不會像原型那樣一路活到函式尾巴。
         實測兩者互有勝負（真實 OOO 模型 dfs 贏 2.6 倍、chain_big greedy 贏 1.2 倍），
         所以預設 auto 兩個都跑。
      3. 配置：直線程式的干涉圖是 interval graph，依起點貪婪著色即是最佳解，
         不需要跑一般的圖著色。
    """
    ops, ff_vals, early_ff = _dce(b, ff_next, outs)
    order_of = {v: i for i, v in enumerate(ops)}
    node_operands = [[a for a in b.op_args(v) if b.is_op(a)] for v in ops]
    node_operands += [[v] for _, v in ff_vals]
    nop = len(ops)

    if scheduler == "auto":
        cands = SCHEDULERS
    elif scheduler in SCHEDULERS:
        cands = (scheduler,)
    else:
        raise BstmError("不認得的排程器 %r（可用：auto, %s）"
                        % (scheduler, ", ".join(SCHEDULERS)))

    best = None
    for name in cands:
        if name == "greedy":
            sched = _sched_greedy(b, ops, ff_vals, node_operands, nop,
                                  order_of, outs)
        else:
            sched = _sched_dfs(b, ops, ff_vals, order_of, outs)
        if len(sched) != len(node_operands):
            raise BstmError("內部錯誤：排程 %s 沒跑完（%d/%d）"
                            % (name, len(sched), len(node_operands)))
        stream = _make_stream(b, ops, ff_vals, early_ff, ff_next, outs, sched, nop)
        nslots, slot_of = _allocate(b, stream)
        if best is None or nslots < best[1]:
            best = (stream, nslots, slot_of, name)
    stream, nslots, slot_of, name = best
    return stream, nslots, slot_of, name


def _dce(b, ff_next, outs):
    """從 sink 倒推，砍掉沒被用到的 op。"""
    live = set()
    stack = list(ff_next.values())
    stack += [v for _, _, v in outs]
    while stack:
        v = stack.pop()
        if v in live or not b.is_op(v):
            continue
        live.add(v)
        for a in b.op_args(v):
            if b.is_op(a) and a not in live:
                stack.append(a)
    ops = [v for v in b.ops if v in live]
    ff_vals, early_ff = [], []
    for k in sorted(ff_next):
        v = ff_next[k]
        if b.is_op(v):
            ff_vals.append((k, v))
        else:
            early_ff.append(k)
    return ops, ff_vals, early_ff


def _sched_greedy(b, ops, ff_vals, node_operands, nop, order_of, outs):
    """暫存器壓力導向的貪婪 list scheduling。"""
    nuse = {}
    for operands in node_operands:
        for a in operands:
            nuse[a] = nuse.get(a, 0) + 1
    for _, _, v in outs:                       # 輸出埠那一份永遠留到最後才扣
        if b.is_op(v):
            nuse[v] = nuse.get(v, 0) + 1

    npred = [0] * len(node_operands)
    succs = {}
    for ni, operands in enumerate(node_operands):
        uniq = set(operands)
        npred[ni] = len(uniq)
        for a in uniq:
            succs.setdefault(a, []).append(ni)

    depth = {}
    for v in ops:
        dmax = 0
        for a in b.op_args(v):
            if b.is_op(a):
                da = depth[a] + 1
                if da > dmax:
                    dmax = da
        depth[v] = dmax

    when = {}

    def cost(ni):
        seen = {}
        for a in node_operands[ni]:
            seen[a] = seen.get(a, 0) + 1
        kills = sum(1 for a, cnt in seen.items() if nuse.get(a, 0) == cnt)
        return (1 if ni < nop else 0) - kills

    def tiebreak(ni):
        # 先比「最近才算出來的值」（locality），再比深度
        rec = max((when.get(a, -1) for a in node_operands[ni]), default=-1)
        if ni >= nop:
            return (-1, -rec, 0, 0)            # state 更新優先：只會減少活值
        v = ops[ni]
        return (0, -rec, -depth[v], order_of[v])

    heap = []
    scheduled = [False] * len(node_operands)
    for ni in range(len(node_operands)):
        if npred[ni] == 0:
            heapq.heappush(heap, (cost(ni), tiebreak(ni), ni))
    sched = []
    step = 0
    while heap:
        c, t, ni = heapq.heappop(heap)
        if scheduled[ni]:
            continue
        rc, rt = cost(ni), tiebreak(ni)
        if (rc, rt) != (c, t):                 # 代價變了 → 重排（lazy update）
            heapq.heappush(heap, (rc, rt, ni))
            continue
        scheduled[ni] = True
        sched.append(ni)
        for a in node_operands[ni]:
            nuse[a] -= 1
        if ni < nop:
            v = ops[ni]
            when[v] = step
            for u in succs.get(v, ()):
                npred[u] -= 1
                if npred[u] == 0:
                    heapq.heappush(heap, (cost(u), tiebreak(u), u))
        step += 1
    return sched


def _sched_dfs(b, ops, ff_vals, order_of, outs):
    """從 sink 往回 DFS post-order：值一算出來馬上就被它的消費者吃掉。

    sink 的順序照「D 值在拓樸序裡的位置」排，這樣同一個模組/同一個暫存器的
    sink 會排在一起，共用的中間值不會被拉很長。
    """
    nop = len(ops)
    order = sorted(range(len(ff_vals)),
                   key=lambda i: order_of.get(ff_vals[i][1], -1))
    done = set()
    sched = []

    def visit(val):
        if not b.is_op(val) or val in done:
            return
        stack = [(val, False)]
        while stack:
            v, expanded = stack.pop()
            if v in done:
                continue
            if expanded:
                done.add(v)
                sched.append(order_of[v])
                continue
            stack.append((v, True))
            for a in b.op_args(v):
                if b.is_op(a) and a not in done:
                    stack.append((a, False))

    for si in order:
        visit(ff_vals[si][1])
        sched.append(nop + si)
    # 只有輸出埠用得到的 cone（沒有任何 state 更新依賴它）補在最後
    for _, _, v in outs:
        visit(v)
    return sched


def _make_stream(b, ops, ff_vals, early_ff, ff_next, outs, sched, nop):
    stream = []
    for k in early_ff:
        stream.append(Instr("ff", ff_next[k], k))
    for ni in sched:
        if ni < nop:
            stream.append(Instr("op", ops[ni]))
        else:
            k, v = ff_vals[ni - nop]
            stream.append(Instr("ff", v, k))
    # 輸出埠留到最後才寫：這樣即使呼叫端把輸入/輸出陣列指到同一塊也不會出錯
    for name, bit, v in outs:
        stream.append(Instr("out", v, (name, bit)))
    return stream


def _allocate(b, stream):
    """liveness（直線程式 → 每個值一段區間）+ 線性掃描配 slot。"""
    last_use = {}
    for i, ins in enumerate(stream):
        if ins.op == "op":
            for a in b.op_args(ins.value):
                if b.is_op(a):
                    last_use[a] = i
        elif b.is_op(ins.value):
            last_use[ins.value] = i

    free = []
    nslots = 0
    slot_of = {}
    expire = {}
    for i, ins in enumerate(stream):
        for v in expire.pop(i, ()):        # 先回收：C 的指派是先讀右邊再寫左邊
            heapq.heappush(free, slot_of[v])
        if ins.op != "op":
            continue
        v = ins.value
        lu = last_use.get(v, i)
        s = heapq.heappop(free) if free else nslots
        if s == nslots:
            nslots += 1
        slot_of[v] = s
        ins.slot = s
        expire.setdefault(lu, []).append(v)
    return nslots, slot_of


def compile_module(module, verbose=False, scheduler="auto"):
    return Compiler(module, verbose=verbose, scheduler=scheduler).run()
