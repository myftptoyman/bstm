#!/bin/bash
# Agent E 後段回歸：cfg_rob_entries 在 最小(W) / 中間 / MAX 三點都要能 commit，
# 而且 retire 數不得因 mask 變大而劣化（CONTRACT v7 §10）。
# 用法: backend/run_regress.sh [tree]   tree 預設 = 本目錄的上一層 (model/ooo)
set -u
TREE=${1:-$(cd "$(dirname "$0")/.." && pwd)}
OBJ=${TMPDIR:-/tmp}/agente_regress_$$
ROBN=$(grep -oP '`define\s+ROB_N\s+\K[0-9]+' "$TREE/common/ifc.vh")
W=$(grep -oP '`define\s+W\s+\K[0-9]+' "$TREE/common/ifc.vh")
echo "=== be_* 回歸  tree=$TREE  ROB_N=$ROBN  W=$W ==="
cd "$TREE" || exit 1
verilator --binary -Wno-fatal -I. --top tb_be backend/tb_be_regress.sv backend/*.v \
          -o simreg --Mdir "$OBJ" >"$OBJ.log" 2>&1 || { echo "  BUILD FAIL"; tail -5 "$OBJ.log"; exit 1; }
FAIL=0
for mode in 0 1 2 3 4 5 6 7 8 9; do
  prev=-1; line=""
  for mask in $W $((ROBN/2)) $ROBN; do
    out=$("$OBJ/simreg" +mode=$mode +rob=$mask 2>&1)
    ret=$(echo "$out" | grep -oP 'retired=\K[0-9]+' | head -1)
    bad=$(echo "$out" | grep -oP 'arf_bad=\K[0-9]+' | head -1)
    line="$line  mask=$mask:ret=$ret"
    [ "${ret:-0}" -eq 0 ]  && { echo "  FAIL mode=$mode mask=$mask 完全沒有 commit"; FAIL=1; }
    [ "${bad:-1}" -ne 0 ]  && { echo "  FAIL mode=$mode mask=$mask cmt_arf 不符 ($bad)"; FAIL=1; }
    # 允許 5% 雜訊，但不得明顯劣化
    if [ "$prev" -ge 0 ] && [ "${ret:-0}" -lt $((prev*95/100)) ]; then
      echo "  FAIL mode=$mode mask 變大反而劣化: $prev -> $ret"; FAIL=1
    fi
    prev=${ret:-0}
  done
  printf "  mode=%-2s %s\n" "$mode" "$line"
done
rm -rf "$OBJ" "$OBJ.log"
[ $FAIL -eq 0 ] && echo "=== 回歸 PASS ===" || echo "=== 回歸 FAIL ==="
exit $FAIL
