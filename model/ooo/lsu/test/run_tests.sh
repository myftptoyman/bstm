#!/bin/bash
# lsu_q 自測：對多組 ifc.vh 尺寸做 lint + yosys + 功能測試
# 用法: model/ooo/lsu/test/run_tests.sh [scratch_dir]
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OOO="$(cd "$HERE/../.." && pwd)"
SCRATCH="${1:-/tmp/agentf_lsu_test}"
FAIL=0
red(){ printf '\033[31m%s\033[0m\n' "$*"; }
grn(){ printf '\033[32m%s\033[0m\n' "$*"; }
ylw(){ printf '\033[33m%s\033[0m\n' "$*"; }

rm -rf "$SCRATCH"; mkdir -p "$SCRATCH/common" "$SCRATCH/lsu/test"
cp "$OOO/lsu/lsu_q.v"            "$SCRATCH/lsu/"
cp "$HERE/tb_lsu_q.cpp" "$HERE/gen_layout.py" "$SCRATCH/lsu/test/"

# 變體： 名稱:ROB_N:ROB_W:PRF_N:PRF_W:LDQ_N:STQ_N:LSQ_W
VARIANTS="
v7-base:64:6:64:6:16:16:4
rob128:128:7:64:6:16:16:4
rob128-prf256:128:7:256:8:16:16:4
prf256:64:6:256:8:16:16:4
lsq8:128:7:256:8:8:8:3
"
for V in $VARIANTS; do
  IFS=: read -r NAME RN RW PN PW LDN STN LW <<< "$V"
  ylw "=== $NAME  (ROB=$RN/$RW  PRF=$PN/$PW  LSQ=$LDN/$STN w=$LW) ==="
  python3 - "$OOO/common/ifc.vh" "$SCRATCH/common/ifc.vh" \
           "$RN" "$RW" "$PN" "$PW" "$LDN" "$STN" "$LW" <<'PYEOF'
import sys, re
src,dst,RN,RW,PN,PW,LDN,STN,LW = sys.argv[1], sys.argv[2], *[int(x) for x in sys.argv[3:]]
s = open(src).read()
P = PW
def setdef(s,k,v):
    return re.sub(r'(?m)^(`define %s)[ \t]+\S+' % k, r'\g<1>  %s' % v, s, count=1)
for k,v in [('ROB_N',RN),('ROB_W',RW),('PRF_N',PN),('PRF_W',PW),
            ('LDQ_N',LDN),('STQ_N',STN),('LSQ_W',LW)]:
    s = setdef(s,k,v)
# RUOP 佈局隨 PRF_W 長高（P=6 時與現行 ifc.vh 逐 bit 相同）
lay = {'RUOP_W': 3*P+30, 'RUOP_D': "%d:2"%(P+1), 'RUOP_DV': P+2,
       'RUOP_S2': "%d:%d"%(2*P+2,P+3), 'RUOP_S2V': 2*P+3,
       'RUOP_S1': "%d:%d"%(3*P+3,2*P+4), 'RUOP_S1V': 3*P+4,
       'RUOP_LAT': "%d:%d"%(3*P+8,3*P+5), 'RUOP_CLASS': "%d:%d"%(3*P+12,3*P+9),
       'RUOP_WRONGPATH': 3*P+13, 'RUOP_ARFD': "%d:%d"%(3*P+18,3*P+14),
       'RUOP_ARFDV': 3*P+19}
for k,v in lay.items(): s = setdef(s,k,v)
open(dst,'w').write(s)
PYEOF

  # --- verilator lint ---
  LG=$(cd "$SCRATCH" && verilator --lint-only -Wall -I. lsu/lsu_q.v 2>&1)
  if [ -n "$LG" ]; then red "  FAIL  lint"; echo "$LG" | head -8; FAIL=$((FAIL+1));
  else grn "  PASS  lint (0 warning)"; fi

  # --- yosys ---
  if command -v docker >/dev/null; then
    YO=$(docker run --rm -v "$SCRATCH":/w -w /w hdlc/yosys:latest yosys -p \
      'read_verilog -I. lsu/lsu_q.v; hierarchy -check -top lsu_q; proc; opt; check -assert; stat; stat -width' 2>&1)
    CELLS=$(echo "$YO" | grep 'Number of cells' | tail -1 | tr -s ' ' | cut -d' ' -f5)
    FLOPS=$(echo "$YO" | grep -oE '\$sdffe?_[0-9]+ +[0-9]+' \
            | awk '{n=$1; sub(/.*_/,"",n); s+=n*$2} END{print s+0}')
    BAD=$(echo "$YO" | grep -cE 'RAMGEM_ASYNC|\$dlatch|\$mem ')
    PROB=$(echo "$YO" | grep -c 'Found and reported 0 problems')
    if [ "$BAD" -gt 0 ] || [ "$PROB" -eq 0 ]; then
      red "  FAIL  yosys (async/latch=$BAD)"; FAIL=$((FAIL+1));
    else grn "  PASS  yosys  cells=$CELLS  flop_bits=$FLOPS"; fi
  fi

  # --- 功能測試 ---
  (cd "$SCRATCH" && python3 lsu/test/gen_layout.py common/ifc.vh lsu/test/layout.h >/dev/null \
   && verilator --cc lsu/lsu_q.v -I. --top-module lsu_q \
        --exe lsu/test/tb_lsu_q.cpp -CFLAGS "-I$SCRATCH/lsu/test -O1" \
        -Wno-fatal --Mdir "$SCRATCH/obj_$NAME" -o sim >/dev/null 2>&1 \
   && make -s -C "$SCRATCH/obj_$NAME" -f Vlsu_q.mk sim >/dev/null 2>&1)
  if [ -x "$SCRATCH/obj_$NAME/sim" ]; then
    OUT=$("$SCRATCH/obj_$NAME/sim" 2>&1); echo "$OUT" | sed 's/^/  /'
    echo "$OUT" | grep -q "ALL PASS" || { red "  FAIL  functional"; FAIL=$((FAIL+1)); }
  else red "  FAIL  build testbench"; FAIL=$((FAIL+1)); fi
done

echo
if [ "$FAIL" -eq 0 ]; then grn "=== lsu_q test suite: ALL VARIANTS PASS ==="; else red "=== lsu_q test suite: $FAIL FAILURE(S) ==="; fi
[ "$FAIL" -eq 0 ]
