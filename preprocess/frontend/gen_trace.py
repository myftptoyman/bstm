#!/usr/bin/env python3
"""
gen_trace.py — 產生自測用的合成指令串流（BSTM Agent A）

程式結構：巢狀迴圈 + 函式呼叫 + 兩個有週期性 pattern 的 conditional branch。
  main:
    outer loop (OUTER 次)
      inner loop (INNER 次)
        alu, alu
        call func            <- call/ret，練 RAS
        cond branch P3       <- 週期 3 的 pattern，bimodal 學不到、TAGE 學得到
        alu * 3
        cond branch P5       <- 週期 5 的 pattern，需要更長的歷史
        alu
        inner loop branch    <- 高度偏向 taken
      outer loop branch
  func: alu, alu, ret

每行格式（frontend format v2，7 欄，與 Agent H 的生產 trace 相同）：
  <pc_hex> <is_branch> <br_type> <taken> <target_hex> <is_block_end> <next_pc_hex>
  br_type     : 0=cond 1=jal 2=jalr 3=ret 4=call
  target_hex  : 解碼出的 architectural 目標，與 taken 無關；非分支為 0
  next_pc_hex : 下一條實際執行指令的 PC；最後一行為 0
"""
import sys

COND, JAL, JALR, RET, CALL = 0, 1, 2, 3, 4


class Emitter:
    """輸出 7 欄格式。用一列 lookahead 來填 next_pc。"""

    def __init__(self, out, fetch_width=4):
        self.out = out
        self.w = fetch_width
        self.n_in_blk = 0
        self.lines = []
        self.pending = None      # (pc, is_br, typ, taken, tgt, blk_end)
        self.count = 0

    def _flush_pending(self, next_pc):
        if self.pending is None:
            return
        pc, is_br, typ, taken, tgt, end = self.pending
        self.lines.append("%x %d %d %d %x %d %x" % (pc, is_br, typ, taken, tgt, end, next_pc))
        self.count += 1
        if len(self.lines) >= 65536:
            self.out.write("\n".join(self.lines) + "\n")
            self.lines = []

    def _put(self, pc, is_br, typ, taken, tgt, blk_end):
        self._flush_pending(pc)
        self.pending = (pc, is_br, typ, taken, tgt, blk_end)

    def alu(self, pc):
        self.n_in_blk += 1
        end = 1 if self.n_in_blk >= self.w else 0
        if end:
            self.n_in_blk = 0
        self._put(pc, 0, 0, 0, 0, end)

    def br(self, pc, typ, taken, tgt):
        """tgt 一律是解碼出的 architectural 目標（not-taken 也要填）。"""
        self.n_in_blk = 0
        self._put(pc, 1, typ, 1 if taken else 0, tgt, 1)

    def flush(self):
        self._flush_pending(0)       # 最後一行 next_pc = 0
        self.pending = None
        if self.lines:
            self.out.write("\n".join(self.lines) + "\n")
            self.lines = []


def gen(out, outer=400, inner=20, fetch_width=4):
    e = Emitter(out, fetch_width)

    MAIN = 0x1000
    OUTER_TOP = 0x1008
    INNER_TOP = 0x1010
    FUNC = 0x2000

    # prologue
    e.alu(MAIN)
    e.alu(MAIN + 4)

    k3 = 0
    k5 = 0
    for o in range(outer):
        e.alu(OUTER_TOP)          # 0x1008
        e.alu(OUTER_TOP + 4)      # 0x100c
        for i in range(inner):
            e.alu(INNER_TOP)          # 0x1010
            e.alu(INNER_TOP + 4)      # 0x1014
            # call func -> 0x2000，回傳位址 0x101c
            e.br(INNER_TOP + 8, CALL, True, FUNC)
            e.alu(FUNC)
            e.alu(FUNC + 4)
            e.br(FUNC + 8, RET, True, INNER_TOP + 0xc)

            # 週期 3 的 conditional branch（target 與 taken 無關，一律填）
            k3 += 1
            t3 = (k3 % 3) == 0
            e.br(INNER_TOP + 0xc, COND, t3, INNER_TOP + 0x20)   # taken -> 0x1030
            if not t3:
                e.alu(INNER_TOP + 0x10)
                e.alu(INNER_TOP + 0x14)
                e.alu(INNER_TOP + 0x18)
                e.alu(INNER_TOP + 0x1c)
            e.alu(INNER_TOP + 0x20)                              # 0x1030

            # 週期 5 的 conditional branch
            k5 += 1
            t5 = (k5 % 5) < 2
            e.br(INNER_TOP + 0x24, COND, t5, INNER_TOP + 0x30)   # taken -> 0x1040
            if not t5:
                e.alu(INNER_TOP + 0x28)
                e.alu(INNER_TOP + 0x2c)
            e.alu(INNER_TOP + 0x30)                              # 0x1040

            # inner loop back edge
            e.br(INNER_TOP + 0x34, COND, i < inner - 1, INNER_TOP)
        # outer loop back edge
        e.alu(INNER_TOP + 0x38)
        e.br(INNER_TOP + 0x3c, COND, o < outer - 1, OUTER_TOP)

    e.alu(INNER_TOP + 0x40)
    e.flush()
    return e.count


def gen_hard(out, iters=2000, fetch_width=4):
    """壓力模式：深度 20 的巢狀呼叫鏈（> RAS 16 entry）+ 輪替目標的 jalr。
    用來涵蓋 RAS 溢位/回捲與間接跳躍路徑。"""
    e = Emitter(out, fetch_width)

    TOP = 0x8000
    CHAIN = 20

    def f(i):
        return 0x4000 + i * 0x40

    def call_chain(i, ret_to):
        e.alu(f(i))
        e.alu(f(i) + 4)
        if i + 1 < CHAIN:
            e.br(f(i) + 8, CALL, True, f(i + 1))      # 回傳位址 f(i)+0xc
            call_chain(i + 1, f(i) + 0xc)
        e.alu(f(i) + 0xc)
        e.br(f(i) + 0x10, RET, True, ret_to)

    IND_TGT = [0x9000, 0x9040, 0x9080, 0x90c0]
    for k in range(iters):
        e.alu(TOP)
        e.alu(TOP + 4)
        e.br(TOP + 8, CALL, True, f(0))               # 回傳位址 TOP+0xc
        call_chain(0, TOP + 0xc)
        e.alu(TOP + 0xc)
        # 間接跳躍，目標在 4 個之間輪替（BTB 只記得上一個 → 高誤預測）
        t = IND_TGT[k % 4]
        e.br(TOP + 0x10, JALR, True, t)
        e.alu(t)
        e.alu(t + 4)
        e.br(t + 8, JAL, True, TOP + 0x20)
        e.alu(TOP + 0x20)
        e.br(TOP + 0x24, COND, k < iters - 1, TOP)

    e.flush()
    return e.count


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--hard":
        iters = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
        n = gen_hard(sys.stdout, iters)
        sys.stderr.write("gen_trace: %d instructions (hard mode, iters=%d)\n" % (n, iters))
    else:
        outer = int(sys.argv[1]) if len(sys.argv) > 1 else 400
        inner = int(sys.argv[2]) if len(sys.argv) > 2 else 20
        n = gen(sys.stdout, outer, inner)
        sys.stderr.write("gen_trace: %d instructions (outer=%d inner=%d)\n" % (n, outer, inner))
