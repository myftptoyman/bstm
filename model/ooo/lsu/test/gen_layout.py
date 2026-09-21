#!/usr/bin/env python3
"""從 common/ifc.vh 抽出 lsu_q 測試需要的欄位佈局，產生 layout.h。
   把「bit 佈局的唯一真相」留在 ifc.vh，測試不自己複製一份。"""
import re, sys, os

src = sys.argv[1] if len(sys.argv) > 1 else \
      os.path.join(os.path.dirname(__file__), '..', '..', 'common', 'ifc.vh')
out = sys.argv[2] if len(sys.argv) > 2 else \
      os.path.join(os.path.dirname(__file__), 'layout.h')

txt = open(src).read()
d = {}
for m in re.finditer(r'(?m)^`define[ \t]+(\w+)[ \t]+(\S+)', txt):
    d[m.group(1)] = m.group(2)

def num(k):
    return int(d[k])

def rng(k):
    """`define X  hi:lo  ->  (lo, width)"""
    v = d[k]
    if ':' in v:
        hi, lo = v.split(':')
        return int(lo), int(hi) - int(lo) + 1
    return int(v), 1

fields = {
    'RUOP_CLASS': rng('RUOP_CLASS'),
    'RUOP_D':     rng('RUOP_D'),
    'RUOP_DV':    rng('RUOP_DV'),
    'DUOP_CLASS': rng('DUOP_CLASS'),
}
sizes = {k: num(k) for k in
         ('W', 'ROB_N', 'ROB_W', 'PRF_N', 'PRF_W', 'LDQ_N', 'STQ_N',
          'LSQ_W', 'MSHR_N', 'MSHR_W', 'RUOP_W', 'DUOP_W')}

with open(out, 'w') as f:
    f.write("// 由 gen_layout.py 從 ifc.vh 自動產生，請勿手改\n#pragma once\n")
    for k, v in sizes.items():
        f.write("static const int L_%s = %d;\n" % (k, v))
    for k, (lo, w) in fields.items():
        f.write("static const int L_%s_LSB = %d, L_%s_W = %d;\n" % (k, lo, k, w))
print("layout.h: " + ", ".join("%s=%d" % (k, v) for k, v in sorted(sizes.items())))
