"""測試用小工具：直接組一張 word-level 網表、產生 C、編起來跑，讀回輸出。

用來測那些「不容易用 Verilog 逼 yosys 產生出來」的 cell（$bmux / $demux /
$xnor / $sshl / $adff ...）。
"""

import ctypes
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def w32(n):
    return "".join("1" if (n >> i) & 1 else "0" for i in range(31, -1, -1))


class NetBuilder(object):
    """最小的網表組裝器：net id 從 2 開始（0/1 留給常數字串）。"""

    def __init__(self, name="t"):
        self.name = name
        self.next_id = 2
        self.ports = {}
        self.cells = {}
        self.n = 0

    def alloc(self, w):
        bits = list(range(self.next_id, self.next_id + w))
        self.next_id += w
        return bits

    def port(self, name, direction, w):
        bits = self.alloc(w)
        self.ports[name] = {"direction": direction, "bits": bits}
        return bits

    def cell(self, ctype, params, conns, outs=("Y",)):
        self.n += 1
        nm = "$%s$t.v:%d$%d" % (ctype.lstrip("$"), self.n, self.n)
        pd = {}
        for k in conns:
            pd[k] = "output" if k in outs else "input"
        self.cells[nm] = {"hide_name": 1, "type": ctype,
                          "parameters": params,
                          "attributes": {"src": "t.v:%d.1-%d.9" % (self.n, self.n)},
                          "port_directions": pd,
                          "connections": conns}
        return nm

    def doc(self):
        return {"creator": "test",
                "modules": {self.name: {"attributes": {"top": 1},
                                        "ports": self.ports,
                                        "cells": self.cells,
                                        "netnames": {}}}}


def run_design(nb, vectors, backend="c64", workdir=None):
    """vectors: list of dict(port -> int)。回傳 list of dict(port -> int)（lane 0）。"""
    tmp = workdir or tempfile.mkdtemp(prefix="bstm_ut_")
    jf = os.path.join(tmp, "m.json")
    with open(jf, "w") as f:
        json.dump(nb.doc(), f)
    cf = os.path.join(tmp, "m.c")
    subprocess.run([sys.executable, os.path.join(ROOT, "bstm-cc"), jf, nb.name,
                    "--backend", backend, "-o", cf, "-q"], check=True)
    clk = set()
    for c in nb.cells.values():
        for cp in ("CLK", "RD_CLK", "WR_CLK"):
            for bb in c["connections"].get(cp, []):
                if isinstance(bb, int):
                    clk.add(bb)
    ins = [(n, len(p["bits"])) for n, p in nb.ports.items()
           if p["direction"] == "input"
           and not (len(p["bits"]) == 1 and p["bits"][0] in clk)]
    outs = [(n, len(p["bits"])) for n, p in nb.ports.items()
            if p["direction"] == "output"]
    L = ["#include <stdio.h>", "int main(void){", "  static state_t a,b;",
         "  bstm_init_state(&a); bstm_init_state(&b);"]
    for n, w in ins:
        L.append("  vec_t i_%s%s;" % (n, "" if w == 1 else "[%d]" % w))
    for n, w in outs:
        L.append("  vec_t o_%s%s;" % (n, "" if w == 1 else "[%d]" % w))
    L.append("  static const unsigned long long vec[%d][%d] = {%s};"
             % (len(vectors), max(1, len(ins)),
                ", ".join("{" + ", ".join("%dULL" % v.get(n, 0) for n, _ in ins) + "}"
                          for v in vectors)))
    L.append("  for (int c=0;c<%d;c++){" % len(vectors))
    for k, (n, w) in enumerate(ins):
        if w == 1:
            L.append("    i_%s = V_BCAST(vec[c][%d] & 1);" % (n, k))
        else:
            L.append("    for(int j=0;j<%d;j++) i_%s[j] = V_BCAST((vec[c][%d]>>j)&1);"
                     % (w, n, k))
    call = ["&b", "&a"] + ["i_" + n for n, _ in ins] + \
           [("&o_" + n) if w == 1 else ("o_" + n) for n, w in outs]
    L.append("    eval_cycle(%s);" % ", ".join(call))
    for n, w in outs:
        if w == 1:
            L.append('    printf("%u ", V_LANE(o_' + n + ',0));')
        else:
            L.append("    { unsigned long long v=0; for(int j=0;j<%d;j++) "
                     'v |= (unsigned long long)V_LANE(o_%s[j],0)<<j; printf("%%llu ", v); }'
                     % (w, n))
    L.append('    printf("\\n"); a=b;')
    L.append("  } return 0; }")
    main = os.path.join(tmp, "main.c")
    with open(cf) as g:
        body = g.read()
    with open(main, "w") as f:
        f.write(body)
        f.write("\n".join(L) + "\n")
    exe = os.path.join(tmp, "m")
    subprocess.run(["gcc", "-O1", "-o", exe, main], check=True)
    txt = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    res = []
    for line in txt.strip().split("\n"):
        vals = [int(x) for x in line.split()]
        res.append({n: vals[i] for i, (n, _) in enumerate(outs)})
    return res
