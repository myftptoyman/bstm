#!/bin/bash
# 建一個 ipc_wp 執行檔。參數是要覆寫的 `define，例如：
#   build_ipc.sh p96 PRF_N=96 PRF_W=7
#   build_ipc.sh all PRF_N=96 PRF_W=7 ROB_N=128 ROB_W=7 IQ_N=64 IQ_W=6 \
#                    LDQ_N=32 STQ_N=32 LSQ_W=5
# 產出 $OUT/r_<name>。ROB/LSQ/IQ 也可以不重建、直接用 runtime cfg 往下調。
set -e
B="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT:-/tmp/bstm_ipc}"; mkdir -p "$OUT"
n=$1; shift
rm -rf "$OUT/v_$n" "$OUT/o_$n" "$OUT/r_$n"
cp -r "$B/model/ooo" "$OUT/v_$n"
[ $# -gt 0 ] && python3 - "$OUT/v_$n/common/ifc.vh" "$@" <<'EOF'
import sys,re
p=sys.argv[1]; s=open(p).read()
for a in sys.argv[2:]:
    k,v=a.split('='); s=re.sub(r'`define %s\s+\S+'%k,'`define %s %s'%(k,v),s)
open(p,'w').write(s)
EOF
d="$OUT/v_$n"
verilator --cc --exe --build -O2 --x-assign 0 --x-initial 0 -Wno-fatal \
  -CFLAGS "-O2 -std=c++17 -I$B/lib/runtime -I$B/include" \
  -LDFLAGS "$B/lib/runtime/libbstm.a -pthread" \
  -I"$d" --top-module ooo_top \
  "$d/top.v" "$d/frontend/fe_front.v" "$d/rename/rn_rename.v" \
  "$d/backend/be_dispatch.v" "$d/backend/be_iq.v" "$d/backend/be_eu.v" \
  "$d/backend/be_rob.v" "$d/lsu/lsu_q.v" \
  "$B/test/tools/ipc_wp.cpp" -o "$OUT/r_$n" --Mdir "$OUT/o_$n" > "$OUT/L_$n.log" 2>&1
[ -f "$OUT/r_$n" ] && echo "$n OK -> $OUT/r_$n" || { echo "$n FAIL"; grep -m3 -E '%Error|error:' "$OUT/L_$n.log"; exit 1; }
