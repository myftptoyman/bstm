#!/usr/bin/env python3
"""Verilator 對拍（PLAN §9.2 第一層驗證）。

同一份 .v：
  * Verilator 編成單 instance 的 C++ 參考模型
  * bstm-cc 編成 bit-sliced C
兩邊餵完全相同的刺激（同一個 xorshift、逐 bit 廣播到所有 lane），
逐 cycle 比對每一個輸出埠的每一個 bit。

「每個 lane 餵一樣的值」還順便驗證了一件事：所有輸出 word 必須是 0 或全 1。
任何 lane 之間互相汙染的 bug（例如位移寫錯方向）都會在這裡當場現形。
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(HERE, "data")
sys.path.insert(0, ROOT)

from bstm_cc import netlist                                    # noqa: E402

PRNG = """
static unsigned long long bstm_rng_state;
static void bstm_rng_seed(unsigned long long s){ bstm_rng_state = s; }
static unsigned bstm_rng_bit(void){
    bstm_rng_state ^= bstm_rng_state << 13;
    bstm_rng_state ^= bstm_rng_state >> 7;
    bstm_rng_state ^= bstm_rng_state << 17;
    return (unsigned)(bstm_rng_state & 1u);
}
"""


def port_list(jsonpath, top):
    m = netlist.load(jsonpath, top)
    ins, outs = [], []
    clk = set()
    for c in m.cells.values():
        for p in ("CLK", "RD_CLK", "WR_CLK"):
            for b in c["connections"].get(p, []):
                if isinstance(b, int):
                    clk.add(b)
    for p in m.ports:
        if p.direction == "input":
            if p.width == 1 and all(isinstance(b, int) and b in clk for b in p.bits):
                continue
            ins.append((p.name, p.width))
        else:
            outs.append((p.name, p.width))
    return ins, outs


def gen_bs_harness(ins, outs, cycles, seed, reset_name):
    L = [PRNG, "#include <stdio.h>", "int main(void){",
         "    static state_t a, b;",
         "    bstm_init_state(&a); bstm_init_state(&b);"]
    for n, w in ins:
        L.append("    vec_t i_%s%s;" % (n, "" if w == 1 else "[%d]" % w))
    for n, w in outs:
        L.append("    vec_t o_%s%s;" % (n, "" if w == 1 else "[%d]" % w))
    L.append("    bstm_rng_seed(%dULL);" % seed)
    L.append("    for (int c = 0; c < %d; c++) {" % cycles)
    for n, w in ins:
        if n == reset_name:
            # 前兩拍一定 reset，之後大約 1/16 的機率再 reset 一次。
            # 這樣才會走到 SRST_VALUE 不是 0 的路徑（原型 gen.py 漏掉的那一條）。
            L.append("        { unsigned r = bstm_rng_bit() & bstm_rng_bit()"
                     " & bstm_rng_bit() & bstm_rng_bit();")
            L.append("          i_%s = V_BCAST(c < 2 || r); }" % n)
            continue
        if w == 1:
            L.append("        i_%s = V_BCAST(bstm_rng_bit());" % n)
        else:
            L.append("        for (int k = 0; k < %d; k++) i_%s[k] = V_BCAST(bstm_rng_bit());"
                     % (w, n))
    call = ["&b", "&a"]
    for n, w in ins:
        call.append("i_" + n)
    for n, w in outs:
        call.append(("&o_" + n) if w == 1 else ("o_" + n))
    L.append("        eval_cycle(%s);" % ", ".join(call))
    L.append('        printf("%d", c);')
    for n, w in outs:
        for i in range(w):
            ref = "o_%s" % n if w == 1 else "o_%s[%d]" % (n, i)
            # 所有 lane 必須一致
            L.append('        { unsigned v0 = V_LANE(%s,0);' % ref)
            L.append("          for (int l = 1; l < BSTM_LANES; l++)")
            L.append('              if (V_LANE(%s,l) != v0) { printf("\\nLANE-MISMATCH %s[%d] cycle %%d lane %%d\\n", c, l); return 3; }'
                     % (ref, n, i))
            L.append('          printf(" %u", v0); }')
    L.append('        printf("\\n");')
    L.append("        a = b;")
    L.append("    }")
    L.append("    return 0;")
    L.append("}")
    return "\n".join(L) + "\n"


def gen_vl_harness(top, ins, outs, cycles, seed, reset_name):
    L = ['#include "V%s.h"' % top, '#include "verilated.h"', "#include <stdio.h>",
         PRNG,
         "int main(int argc, char **argv){",
         "    Verilated::commandArgs(argc, argv);",
         "    V%s *dut = new V%s;" % (top, top),
         "    bstm_rng_seed(%dULL);" % seed,
         "    dut->clk = 0;",
         "    for (int c = 0; c < %d; c++) {" % cycles]
    for n, w in ins:
        if n == reset_name:
            L.append("        { unsigned r = bstm_rng_bit() & bstm_rng_bit()"
                     " & bstm_rng_bit() & bstm_rng_bit();")
            L.append("          dut->%s = (c < 2 || r); }" % n)
            continue
        if w == 1:
            L.append("        dut->%s = bstm_rng_bit();" % n)
        elif w <= 64:
            L.append("        { unsigned long long v = 0;")
            L.append("          for (int k = 0; k < %d; k++) v |= (unsigned long long)bstm_rng_bit() << k;"
                     % w)
            L.append("          dut->%s = v; }" % n)
        else:
            # > 64 bit 的埠在 Verilator 是 VlWide（32-bit word 陣列）
            L.append("        { for (int k = 0; k < %d; k++) dut->%s[k] = 0;"
                     % ((w + 31) // 32, n))
            L.append("          for (int k = 0; k < %d; k++) dut->%s[k>>5] |= ((unsigned)bstm_rng_bit()) << (k & 31); }"
                     % (w, n))
    L.append("        dut->eval();")
    L.append('        printf("%d", c);')
    for n, w in outs:
        for i in range(w):
            if w <= 64:
                L.append('        printf(" %%u", (unsigned)((dut->%s >> %d) & 1));'
                         % (n, i))
            else:
                L.append('        printf(" %%u", (unsigned)((dut->%s[%d] >> %d) & 1));'
                         % (n, i >> 5, i & 31))
    L.append('        printf("\\n");')
    L.append("        dut->clk = 1; dut->eval();")
    L.append("        dut->clk = 0; dut->eval();")
    L.append("    }")
    L.append("    delete dut; return 0;")
    L.append("}")
    return "\n".join(L) + "\n"


def run(vfiles, top, jsonpath, workdir, cycles=400, seed=0x2545F4914F6CDD1D,
        backend="c64", reset_name="rst", vl_extra=(), vdir=None, cc_opt="-O1"):
    os.makedirs(workdir, exist_ok=True)
    ins, outs = port_list(jsonpath, top)
    if not any(n == reset_name for n, _ in ins):
        reset_name = None

    # ---- bit-sliced 側
    bsc = os.path.join(workdir, "model.c")
    subprocess.run([sys.executable, os.path.join(ROOT, "bstm-cc"), jsonpath, top,
                    "--backend", backend, "--no-comments", "-o", bsc, "-q"],
                   check=True)
    harness = os.path.join(workdir, "bs_main.c")
    with open(bsc) as g:
        model = g.read()
    with open(harness, "w") as f:
        f.write(model)
        f.write(gen_bs_harness(ins, outs, cycles, seed, reset_name))
    exe = os.path.join(workdir, "bs")
    subprocess.run(["gcc", cc_opt, "-o", exe, harness], check=True)
    bs_out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout

    # ---- Verilator 側
    vmain = os.path.join(workdir, "vl_main.cpp")
    with open(vmain, "w") as f:
        f.write(gen_vl_harness(top, ins, outs, cycles, seed, reset_name))
    mdir = os.path.join(workdir, "obj_vl")
    base = vdir or DATA
    subprocess.run(["verilator", "--cc", "--build", "--exe", "-Wno-fatal",
                    "--x-assign", "0", "--x-initial", "0"] + list(vl_extra) +
                   ["--top-module", top, "--Mdir", mdir, "-o", "vl",
                    "-CFLAGS", "-O1"] +
                   [os.path.join(base, v) for v in vfiles] + [vmain],
                   check=True, capture_output=True)
    vl_out = subprocess.run([os.path.join(mdir, "vl")], check=True,
                            capture_output=True, text=True).stdout
    return bs_out, vl_out, ins, outs


def compare(bs_out, vl_out, outs, label=""):
    bs = bs_out.strip().split("\n")
    vl = vl_out.strip().split("\n")
    names = []
    for n, w in outs:
        for i in range(w):
            names.append(n if w == 1 else "%s[%d]" % (n, i))
    if len(bs) != len(vl):
        return "%s: cycle 數不一樣 bit-sliced=%d verilator=%d\n%s" % (
            label, len(bs), len(vl), bs_out[:400])
    for c, (x, y) in enumerate(zip(bs, vl)):
        if x != y:
            xs, ys = x.split(), y.split()
            bad = [names[i - 1] for i in range(1, min(len(xs), len(ys)))
                   if xs[i] != ys[i]]
            return ("%s: cycle %s 不一致，差在 %s\n  bit-sliced: %s\n  verilator : %s"
                    % (label, x.split()[0], ", ".join(bad) or "?", x, y))
    return None


if __name__ == "__main__":
    import tempfile
    from synth import synth
    top = sys.argv[1]
    vfiles = sys.argv[2:]
    extra = "memory_dff; opt_clean" if top == "memtest" else ""
    j = synth(vfiles, top, extra=extra)
    wd = tempfile.mkdtemp(prefix="bstm_cosim_")
    bs, vl, ins, outs = run(vfiles, top, j, wd)
    err = compare(bs, vl, outs, top)
    print(err if err else "%s: %d cycle 全部 bit-exact ✓" % (top, len(bs.strip().split())))
    sys.exit(1 if err else 0)
