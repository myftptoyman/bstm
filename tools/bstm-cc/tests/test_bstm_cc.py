"""bstm-cc 自測。`python3 -m pytest` 或 `python3 -m unittest` 都能跑。

需要外部工具的測試會自動 skip：
  * gcc         —— 編譯/執行產生的 C
  * docker+yosys—— 重新合成 .v
  * verilator   —— 逐 cycle 對拍
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(HERE, "data")
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

from bstm_cc import backend as be           # noqa: E402
from bstm_cc import cells, ir, netlist      # noqa: E402
from bstm_cc.compiler import compile_module  # noqa: E402
from bstm_cc.errors import UnsupportedCell   # noqa: E402
import util                                  # noqa: E402
import synth                                 # noqa: E402

HAVE_GCC = shutil.which("gcc") is not None
HAVE_VL = shutil.which("verilator") is not None
HAVE_YS = synth.have_yosys()

# prototype/gen.py 在同一份 chain.json 上跑 cbench 的刺激所得的 checksum。
# 升級後必須一模一樣。
GOLDEN_CHAIN_SINK = "aa38064b48fd97c0"


def load_chain():
    return netlist.load(os.path.join(DATA, "chain.json"), "chain")


# ------------------------------------------------------------------ IR 化簡

class TestSimplify(unittest.TestCase):
    def setUp(self):
        self.b = ir.Builder()
        self.x = self.b.port("x", 0)
        self.y = self.b.port("y", 0)

    def test_const_fold(self):
        b, x = self.b, self.x
        self.assertEqual(b.v_and(x, ir.CONST0), ir.CONST0)
        self.assertEqual(b.v_and(x, ir.CONST1), x)
        self.assertEqual(b.v_or(x, ir.CONST1), ir.CONST1)
        self.assertEqual(b.v_or(x, ir.CONST0), x)
        self.assertEqual(b.v_xor(x, ir.CONST0), x)
        self.assertEqual(b.v_xor(x, x), ir.CONST0)
        self.assertEqual(b.v_not(b.v_not(x)), x)
        self.assertEqual(b.v_and(x, b.v_not(x)), ir.CONST0)
        self.assertEqual(b.v_or(x, b.v_not(x)), ir.CONST1)

    def test_mux_collapse(self):
        """原型會產生 (VZERO & ~S)|(B & S)，這裡必須直接塌成 B & S。"""
        b, x, y = self.b, self.x, self.y
        self.assertEqual(b.v_mux(x, ir.CONST0, y), b.v_and(y, x))
        self.assertEqual(b.v_mux(ir.CONST0, x, y), x)
        self.assertEqual(b.v_mux(ir.CONST1, x, y), y)
        self.assertEqual(b.v_mux(x, y, y), y)
        # s ? ~b : b  →  ~(b ^ s)
        self.assertEqual(b.v_mux(x, b.v_not(y), y), b.v_not(b.v_xor(y, x)))

    def test_cse(self):
        b, x, y = self.b, self.x, self.y
        self.assertEqual(b.v_and(x, y), b.v_and(y, x))
        n = len(b.ops)
        b.v_and(x, y)
        self.assertEqual(len(b.ops), n)

    def test_de_morgan(self):
        b, x, y = self.b, self.x, self.y
        self.assertEqual(b.v_and(b.v_not(x), b.v_not(y)),
                         b.v_not(b.v_or(x, y)))

    def test_maj3(self):
        b, x, y = self.b, self.x, self.y
        self.assertEqual(b.v_maj3(x, y, ir.CONST0), b.v_and(x, y))
        self.assertEqual(b.v_maj3(x, y, ir.CONST1), b.v_or(x, y))
        self.assertEqual(b.v_maj3(x, b.v_not(x), y), y)

    def test_andn_used(self):
        b, x, y = self.b, self.x, self.y
        v = b.v_and(x, b.v_not(y))
        self.assertEqual(b.op_kind(v), ir.ANDN)


# ------------------------------------------------------------------ liveness

class TestLiveness(unittest.TestCase):
    def test_chain_compression(self):
        d = compile_module(load_chain())
        st = d.stats
        ratio = float(st["nets"]) / st["slots"]
        self.assertGreaterEqual(ratio, 20.0,
                                "net→slot 壓縮比 %.1fx 沒到 20x" % ratio)
        self.assertLess(st["slots"], 100)

    def test_slots_are_dense(self):
        d = compile_module(load_chain())
        used = set(i.slot for i in d.stream if i.op == "op")
        self.assertEqual(used, set(range(d.nslots)))

    def test_no_slot_conflict(self):
        """同一個 slot 上的兩個值，生命期不可以重疊。"""
        d = compile_module(load_chain())
        b = d.builder
        last = {}
        for i, ins in enumerate(d.stream):
            if ins.op == "op":
                for a in b.op_args(ins.value):
                    if b.is_op(a):
                        last[a] = i
            elif b.is_op(ins.value):
                last[ins.value] = i
        owner = {}
        for i, ins in enumerate(d.stream):
            if ins.op != "op":
                continue
            prev = owner.get(ins.slot)
            if prev is not None:
                self.assertLessEqual(last[prev], i,
                                     "slot %d 被重複使用但生命期重疊" % ins.slot)
            owner[ins.slot] = ins.value

    @unittest.skipUnless(os.path.exists(os.path.join(DATA, "chain_big.json")),
                         "沒有 chain_big.json")
    def test_big_design(self):
        m = netlist.load(os.path.join(DATA, "chain_big.json"), "chain")
        d = compile_module(m)
        ratio = float(d.stats["nets"]) / d.stats["slots"]
        self.assertGreater(ratio, 100.0)
        # 工作集必須小到塞得進 L1/L2（PLAN §10.2）
        self.assertLess(d.stats["slots"] * 64 // 8, 64 * 1024)


# ------------------------------------------------------------------ 錯誤處理

class TestErrors(unittest.TestCase):
    def test_mul_rejected(self):
        m = netlist.load(os.path.join(DATA, "badmul.json"), "badmul")
        with self.assertRaises(UnsupportedCell) as cm:
            compile_module(m)
        msg = str(cm.exception)
        self.assertIn("$mul", msg)
        self.assertIn("badmul.v:3", msg)      # 來源行號
        self.assertIn("規則 1", msg)

    def test_async_read_rejected(self):
        m = netlist.load(os.path.join(DATA, "asyncrd.json"), "asyncrd")
        with self.assertRaises(UnsupportedCell) as cm:
            compile_module(m)
        msg = str(cm.exception)
        self.assertIn("$memrd", msg)
        self.assertIn("asyncrd.v:12", msg)
        self.assertIn("posedge", msg)         # 有給改寫建議

    def test_unknown_cell(self):
        nb = util.NetBuilder("t")
        a = nb.port("a", "input", 1)
        y = nb.port("y", "output", 1)
        nb.cell("$weird", {"A_WIDTH": util.w32(1), "Y_WIDTH": util.w32(1)},
                {"A": a, "Y": y})
        m = netlist.Module("t", nb.doc()["modules"]["t"])
        with self.assertRaises(UnsupportedCell) as cm:
            compile_module(m)
        self.assertIn("$weird", str(cm.exception))
        self.assertIn("t.v:1", str(cm.exception))


# ------------------------------------------------------------------ backend

class TestBackends(unittest.TestCase):
    def _emit(self, name):
        d = compile_module(load_chain())
        return be.Emitter(d, be.make(name), comments=False).emit()

    def test_c64(self):
        t = self._emit("c64")
        self.assertIn("typedef uint64_t vec_t", t)
        self.assertIn("#define BSTM_LANES 64", t)

    def test_neon(self):
        t = self._emit("neon")
        self.assertIn("uint32x4_t", t)
        self.assertIn("vbslq_u32", t)         # mux 走 BSL
        self.assertIn("#define BSTM_LANES 128", t)

    def test_avx512(self):
        t = self._emit("avx512")
        self.assertIn("__m512i", t)
        self.assertIn("_mm512_ternarylogic_epi32", t)
        self.assertIn("#define BSTM_LANES 512", t)

    def test_lane_mismatch_rejected(self):
        from bstm_cc.errors import BstmError
        d = compile_module(load_chain())
        with self.assertRaises(BstmError):
            be.Emitter(d, be.make("c64"), lanes=128)

    @unittest.skipUnless(HAVE_GCC, "需要 gcc")
    def test_avx512_syntax(self):
        t = self._emit("avx512")
        tmp = tempfile.mkdtemp(prefix="bstm_av_")
        f = os.path.join(tmp, "m.c")
        with open(f, "w") as fh:
            fh.write(t)
        r = subprocess.run(["gcc", "-mavx512f", "-fsyntax-only", f],
                           capture_output=True, text=True)
        if "avx512" in r.stderr and "not supported" in r.stderr:
            self.skipTest("這台機器的 gcc 不支援 avx512")
        self.assertEqual(r.returncode, 0, r.stderr[:2000])


# ------------------------------------------------------------------ 產生的 C

@unittest.skipUnless(HAVE_GCC, "需要 gcc")
class TestGeneratedC(unittest.TestCase):
    def test_chain_matches_prototype(self):
        """跟原型 gen.py 的 checksum 必須完全一樣（驗收條件 1）。"""
        tmp = tempfile.mkdtemp(prefix="bstm_bench_")
        c = os.path.join(tmp, "m.c")
        subprocess.run([sys.executable, os.path.join(ROOT, "bstm-cc"),
                        os.path.join(DATA, "chain.json"), "chain", "-o", c, "-q"],
                       check=True)
        main = os.path.join(tmp, "b.c")
        with open(c) as g:
            model = g.read()
        with open(os.path.join(HERE, "bench_main.c")) as g:
            drv = g.read()
        with open(main, "w") as f:
            f.write(model)
            f.write("#define BENCH_N 200000\n")
            f.write(drv)
        exe = os.path.join(tmp, "b")
        subprocess.run(["gcc", "-O2", "-o", exe, main], check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
        self.assertIn("sink", out)
        # checksum 是逐 cycle 累積的，200000 拍跟 2000000 拍不同；
        # 這裡只確認能跑且不是 0
        self.assertNotIn("sink 0000000000000000", out)

    def test_chain_golden_sink(self):
        tmp = tempfile.mkdtemp(prefix="bstm_gold_")
        c = os.path.join(tmp, "m.c")
        subprocess.run([sys.executable, os.path.join(ROOT, "bstm-cc"),
                        os.path.join(DATA, "chain.json"), "chain", "-o", c, "-q"],
                       check=True)
        main = os.path.join(tmp, "b.c")
        with open(c) as g:
            model = g.read()
        with open(os.path.join(HERE, "bench_main.c")) as g:
            drv = g.read()
        with open(main, "w") as f:
            f.write(model)
            f.write(drv)
        exe = os.path.join(tmp, "b")
        subprocess.run(["gcc", "-O2", "-o", exe, main], check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
        self.assertIn(GOLDEN_CHAIN_SINK, out,
                      "checksum 跟原型不一致:\n" + out)


# ------------------------------------------------------------------ 合成網表上的 cell 語意

@unittest.skipUnless(HAVE_GCC, "需要 gcc")
class TestCellSemantics(unittest.TestCase):
    def test_bmux(self):
        nb = util.NetBuilder("t")
        a = nb.port("a", "input", 16)          # 4 段 x 4 bit
        s = nb.port("s", "input", 2)
        y = nb.port("y", "output", 4)
        nb.cell("$bmux", {"WIDTH": util.w32(4), "S_WIDTH": util.w32(2)},
                {"A": a, "S": s, "Y": y})
        vec = [{"a": 0xFED8, "s": k} for k in range(4)]
        got = util.run_design(nb, vec)
        for k in range(4):
            self.assertEqual(got[k]["y"], (0xFED8 >> (4 * k)) & 0xF)

    def test_demux(self):
        nb = util.NetBuilder("t")
        a = nb.port("a", "input", 4)
        s = nb.port("s", "input", 2)
        y = nb.port("y", "output", 16)
        nb.cell("$demux", {"WIDTH": util.w32(4), "S_WIDTH": util.w32(2)},
                {"A": a, "S": s, "Y": y})
        vec = [{"a": 0xB, "s": k} for k in range(4)]
        got = util.run_design(nb, vec)
        for k in range(4):
            self.assertEqual(got[k]["y"], 0xB << (4 * k))

    def test_xnor_and_reduce_xnor(self):
        nb = util.NetBuilder("t")
        a = nb.port("a", "input", 4)
        b = nb.port("b", "input", 4)
        y = nb.port("y", "output", 4)
        z = nb.port("z", "output", 1)
        nb.cell("$xnor", {"A_WIDTH": util.w32(4), "B_WIDTH": util.w32(4),
                          "Y_WIDTH": util.w32(4), "A_SIGNED": util.w32(0),
                          "B_SIGNED": util.w32(0)},
                {"A": a, "B": b, "Y": y})
        nb.cell("$reduce_xnor", {"A_WIDTH": util.w32(4), "Y_WIDTH": util.w32(1),
                                 "A_SIGNED": util.w32(0)},
                {"A": a, "Y": z})
        vec = [{"a": 0x9, "b": 0x5}, {"a": 0xF, "b": 0x0}, {"a": 0x7, "b": 0x7}]
        got = util.run_design(nb, vec)
        for v, g in zip(vec, got):
            self.assertEqual(g["y"], (~(v["a"] ^ v["b"])) & 0xF)
            par = bin(v["a"]).count("1") & 1
            self.assertEqual(g["z"], 1 - par)

    def test_sshl_and_pos(self):
        nb = util.NetBuilder("t")
        a = nb.port("a", "input", 8)
        sh = nb.port("sh", "input", 3)
        y = nb.port("y", "output", 8)
        z = nb.port("z", "output", 8)
        nb.cell("$sshl", {"A_WIDTH": util.w32(8), "B_WIDTH": util.w32(3),
                          "Y_WIDTH": util.w32(8), "A_SIGNED": util.w32(1),
                          "B_SIGNED": util.w32(0)},
                {"A": a, "B": sh, "Y": y})
        nb.cell("$pos", {"A_WIDTH": util.w32(4), "Y_WIDTH": util.w32(8),
                         "A_SIGNED": util.w32(1)},
                {"A": a[:4], "Y": z})
        vec = [{"a": 0x93, "sh": k} for k in range(8)]
        got = util.run_design(nb, vec)
        for k in range(8):
            self.assertEqual(got[k]["y"], (0x93 << k) & 0xFF)
            self.assertEqual(got[k]["z"], 0x03)      # 0x93 的低 4 bit = 0011，正號 -> 補 0

    def test_shift_overflow(self):
        """位移量大於資料寬度時必須整個移光，而不是環繞。"""
        nb = util.NetBuilder("t")
        a = nb.port("a", "input", 4)
        sh = nb.port("sh", "input", 5)
        y = nb.port("y", "output", 4)
        z = nb.port("z", "output", 4)
        nb.cell("$shl", {"A_WIDTH": util.w32(4), "B_WIDTH": util.w32(5),
                         "Y_WIDTH": util.w32(4), "A_SIGNED": util.w32(0),
                         "B_SIGNED": util.w32(0)},
                {"A": a, "B": sh, "Y": y})
        nb.cell("$sshr", {"A_WIDTH": util.w32(4), "B_WIDTH": util.w32(5),
                          "Y_WIDTH": util.w32(4), "A_SIGNED": util.w32(1),
                          "B_SIGNED": util.w32(0)},
                {"A": a, "B": sh, "Y": z})
        vec = [{"a": 0xB, "sh": k} for k in (0, 1, 3, 4, 7, 17, 31)]
        got = util.run_design(nb, vec)
        for g, v in zip(got, vec):
            k = v["sh"]
            self.assertEqual(g["y"], (0xB << k) & 0xF)
            self.assertEqual(g["z"], 0xF if k >= 4 else ((0xB >> k) | (0xF << (4 - k))) & 0xF)

    def test_adff_as_sync(self):
        """$adff 目前當成同步 reset（會發警告），這裡確認行為。"""
        nb = util.NetBuilder("t")
        d = nb.port("d", "input", 4)
        r = nb.port("r", "input", 1)
        clk = nb.port("clk", "input", 1)
        q = nb.port("q", "output", 4)
        qn = nb.alloc(4)
        nb.cell("$adff", {"WIDTH": util.w32(4), "CLK_POLARITY": util.w32(1),
                          "ARST_POLARITY": util.w32(1), "ARST_VALUE": "1010"},
                {"CLK": clk, "ARST": r, "D": d, "Q": qn}, outs=("Q",))
        nb.ports["q"]["bits"] = qn
        vec = [{"d": 5, "r": 0}, {"d": 6, "r": 0}, {"d": 7, "r": 1}, {"d": 8, "r": 0}]
        got = util.run_design(nb, vec)
        self.assertEqual([g["q"] for g in got], [0, 5, 6, 0xA])

    def test_signed_compare(self):
        nb = util.NetBuilder("t")
        a = nb.port("a", "input", 4)
        b = nb.port("b", "input", 4)
        lt = nb.port("lt", "output", 1)
        ltu = nb.port("ltu", "output", 1)
        nb.cell("$lt", {"A_WIDTH": util.w32(4), "B_WIDTH": util.w32(4),
                        "Y_WIDTH": util.w32(1), "A_SIGNED": util.w32(1),
                        "B_SIGNED": util.w32(1)}, {"A": a, "B": b, "Y": lt})
        nb.cell("$lt", {"A_WIDTH": util.w32(4), "B_WIDTH": util.w32(4),
                        "Y_WIDTH": util.w32(1), "A_SIGNED": util.w32(0),
                        "B_SIGNED": util.w32(0)}, {"A": a, "B": b, "Y": ltu})
        vec = [{"a": x, "b": y} for x in range(16) for y in range(16)]
        got = util.run_design(nb, vec)
        for v, g in zip(vec, got):
            sx = v["a"] - 16 if v["a"] >= 8 else v["a"]
            sy = v["b"] - 16 if v["b"] >= 8 else v["b"]
            self.assertEqual(g["lt"], int(sx < sy), str(v))
            self.assertEqual(g["ltu"], int(v["a"] < v["b"]), str(v))


# ------------------------------------------------------------------ Verilator 對拍

@unittest.skipUnless(HAVE_GCC and HAVE_VL and HAVE_YS,
                     "需要 gcc + verilator + docker/yosys")
class TestCosim(unittest.TestCase):
    def _cosim(self, top, vfiles, extra="", cycles=300):
        import cosim
        j = synth.synth(vfiles, top, extra=extra)
        wd = tempfile.mkdtemp(prefix="bstm_cosim_")
        bs, vl, ins, outs = cosim.run(vfiles, top, j, wd, cycles=cycles)
        err = cosim.compare(bs, vl, outs, top)
        self.assertIsNone(err, err)

    def test_ops(self):
        self._cosim("ops", ["ops.v"])

    def test_chain(self):
        self._cosim("chain", ["stage.v", "chain.v"])

    def test_memtest(self):
        self._cosim("memtest", ["memtest.v"], extra="memory_dff; opt_clean")


if __name__ == "__main__":
    unittest.main(verbosity=2)


# ------------------------------------------------------------------ 同步 reset 值（原型的 bug）

def expected_reset_state(jsonpath, top):
    """從網表直接算出「rst 拉高一拍之後」每個 state bit 應該是什麼。

    prototype/gen.py 第 156 行算了 rv="VZERO" 卻從來沒用，所以所有 $sdff/$sdffe
    的 SRST_VALUE 都被當成 0。stage.v 的 credit 是 reset 成 {CREDIT_W{1'b1}}=15，
    原型會給 0 —— 這個函式就是拿來抓這件事的。
    """
    with open(jsonpath) as f:
        m = json.load(f)["modules"][top]
    want = {}
    for c in m["cells"].values():
        t = c["type"]
        if t not in ("$sdff", "$sdffe", "$sdffce", "$adff", "$adffe"):
            continue
        q = c["connections"]["Q"]
        key = "SRST_VALUE" if t.startswith("$sdff") else "ARST_VALUE"
        rv = netlist.pbits(c, key, len(q))
        for i, bb in enumerate(q):
            if isinstance(bb, int):
                want[bb] = rv[i]
    order = sorted(want)
    return [want[b] for b in order], order


@unittest.skipUnless(HAVE_GCC, "需要 gcc")
class TestResetValue(unittest.TestCase):
    def test_srst_value_honoured(self):
        j = os.path.join(DATA, "chain.json")
        want, order = expected_reset_state(j, "chain")
        self.assertIn(1, want, "測試網表裡沒有 reset 成非 0 的暫存器，這個測試就沒意義")
        tmp = tempfile.mkdtemp(prefix="bstm_rst_")
        c = os.path.join(tmp, "m.c")
        subprocess.run([sys.executable, os.path.join(ROOT, "bstm-cc"), j, "chain",
                        "-o", c, "-q"], check=True)
        drv = """
#include <stdio.h>
int main(void){
    static state_t a, b;
    vec_t rdy, ov, tag[6], it[6] = {0}, il[4] = {0};
    bstm_init_state(&a); bstm_init_state(&b);
    /* rst 拉高一拍 */
    eval_cycle(&b, &a, VONES, VONES, it, il, VONES, &rdy, &ov, tag);
    for (int i = 0; i < BSTM_NSTATE; i++) putchar('0' + V_LANE(b.q[i], 0));
    putchar('\\n');
    /* 再跑一拍不 reset，reset 值必須留著（credit 不會自己歸零） */
    a = b;
    eval_cycle(&b, &a, VZERO, VZERO, it, il, VZERO, &rdy, &ov, tag);
    for (int i = 0; i < BSTM_NSTATE; i++) putchar('0' + V_LANE(b.q[i], 0));
    putchar('\\n');
    return 0;
}
"""
        main = os.path.join(tmp, "d.c")
        with open(c) as g:
            model = g.read()
        with open(main, "w") as f:
            f.write(model)
            f.write(drv)
        exe = os.path.join(tmp, "d")
        subprocess.run(["gcc", "-O1", "-o", exe, main], check=True)
        out = subprocess.run([exe], check=True, capture_output=True,
                             text=True).stdout.strip().split("\n")
        got = [int(ch) for ch in out[0]]
        self.assertEqual(len(got), len(want))
        bad = [(order[i], want[i], got[i]) for i in range(len(want))
               if want[i] != got[i]]
        self.assertEqual(bad, [],
                         "reset 後有 %d 個 state bit 不對（net, 期望, 實得）: %s"
                         % (len(bad), bad[:8]))
        # 被 reset 成 1 的位元，在下一拍沒有 enable 的情況下必須維持 1
        ones = [i for i, v in enumerate(want) if v]
        got2 = [int(ch) for ch in out[1]]
        self.assertTrue(all(got2[i] == 1 for i in ones),
                        "reset 值在下一拍就掉了")


# ------------------------------------------------------------------ 真實 OOO 模型

OOO_JSON = os.path.join(DATA, "ooo", "ooo_top.json")
OOO_DIR = os.path.join(DATA, "ooo")
OOO_V = ["top.v", "frontend/fe_front.v", "rename/rn_rename.v",
         "backend/be_dispatch.v", "backend/be_iq.v", "backend/be_eu.v",
         "backend/be_rob.v", "lsu/lsu_q.v"]


@unittest.skipUnless(os.path.exists(OOO_JSON),
                     "沒有真實模型的網表（跑 make ooo 產生）")
class TestRealModel(unittest.TestCase):
    """PLAN §10.2 的關鍵閘：真實規模的模型，工作集必須塞得進 L2。"""

    def test_working_set_fits_l2(self):
        d = compile_module(netlist.load(OOO_JSON, "ooo_top"))
        st = d.stats
        ws = st["slots"] * 64 // 8
        ratio = float(st["nets"]) / st["slots"]
        self.assertGreaterEqual(ratio, 20.0,
                                "net→slot 壓縮比只有 %.1fx" % ratio)
        self.assertLess(ws, 512 * 1024,
                        "工作集 %d bytes 超過 512 KB，塞不進 L2" % ws)

    @unittest.skipUnless(HAVE_GCC and HAVE_VL, "需要 gcc + verilator")
    def test_bit_exact_vs_verilator(self):
        import cosim
        wd = tempfile.mkdtemp(prefix="bstm_ooo_")
        bs, vl, ins, outs = cosim.run(
            OOO_V, "ooo_top", OOO_JSON, wd, cycles=120,
            vl_extra=["-Wno-WIDTH", "-I" + OOO_DIR], vdir=OOO_DIR, cc_opt="-O0")
        err = cosim.compare(bs, vl, outs, "ooo_top")
        self.assertIsNone(err, err)
