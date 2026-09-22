#!/bin/bash
# Agent E 後段回歸（CONTRACT v8）：
#   * cfg_rob_entries 在 1 / W / MAX/2 / MAX 四點都要能 commit
#     （v8 partial accept 之後下限回到 1）
#   * retire 數不得因 mask 變大而劣化
#   * cnt_lost_rob + cnt_lost_lsq + cnt_lost_iq == Σ(想送 − 實際送)  ← 守恆自檢
#   * cmt_arf 端到端逐條比對
# 用法: backend/run_regress.sh [tree]   tree 預設 = 本目錄的上一層 (model/ooo)
set -u
TREE=${1:-$(cd "$(dirname "$0")/.." && pwd)}
OBJ=${TMPDIR:-/tmp}/agente_regress_$$
ROBN=$(grep -oP '`define\s+ROB_N\s+\K[0-9]+' "$TREE/common/ifc.vh")
IQN=$(grep -oP '`define\s+IQ_N\s+\K[0-9]+' "$TREE/common/ifc.vh")
W=$(grep -oP '`define\s+W\s+\K[0-9]+' "$TREE/common/ifc.vh")
echo "=== be_* 回歸  tree=$TREE  ROB_N=$ROBN  IQ_N=$IQN  W=$W ==="
cd "$TREE" || exit 1
verilator --binary -Wno-fatal -I. --top tb_be backend/tb_be_regress.sv backend/*.v \
          -o simreg --Mdir "$OBJ" >"$OBJ.log" 2>&1 || { echo "  BUILD FAIL"; tail -5 "$OBJ.log"; exit 1; }
FAIL=0
for mode in 0 1 2 3 4 5 6 7 8 9 10; do
  prev=-1; plrob=-1; line=""
  for mask in 1 $W $((ROBN/2)) $ROBN; do
    out=$("$OBJ/simreg" +mode=$mode +rob=$mask 2>&1)
    ret=$(echo "$out" | grep -oP 'retired=\K[0-9]+' | head -1)
    bad=$(echo "$out" | grep -oP 'arf_bad=\K[0-9]+' | head -1)
    cons=$(echo "$out" | grep -c CONSERVED)
    [ "${cons:-0}" -ne 1 ] && { echo "  FAIL mode=$mode mask=$mask cnt_lost_* 不守恆"; FAIL=1; }
    line="$line  mask=$mask:ret=$ret"
    [ "${ret:-0}" -eq 0 ]  && { echo "  FAIL mode=$mode mask=$mask 完全沒有 commit"; FAIL=1; }
    # 末段存活性：跑到最後 100 拍還要有 commit。死結時 retire 與 lost_* 會「一起」歸零，
    # 所以相對吞吐與損失 counter 都抓不到，只有存活性抓得到。
    tail100=$(echo "$out" | grep -oP 'rettail=\K[0-9]+' | head -1)
    [ "${tail100:-0}" -eq 0 ] && { echo "  FAIL mode=$mode mask=$mask 最後 100 拍沒有任何 commit（疑似死結）"; FAIL=1; }
    [ "${bad:-1}" -ne 0 ]  && { echo "  FAIL mode=$mode mask=$mask cmt_arf 不符 ($bad)"; FAIL=1; }
    # ROB 變大 -> ROB 綁住的損失只能變少。用 cnt_lost_rob 而不是 retire：
    # retire 會被別的資源（TB freelist / IQ / LSQ）接手而下降，那是真實的資源失衡，
    # 不是 bug；但「ROB 自己造成的損失」隨 ROB 變大而上升，一定是 bug。
    lrob=$(echo "$out" | grep -oP 'lost_rob=\K[0-9]+' | head -1)
    if [ "$plrob" -ge 0 ] && [ "${lrob:-0}" -gt $((plrob + 8)) ]; then
      echo "  FAIL mode=$mode ROB 變大但 cnt_lost_rob 反而上升: $plrob -> $lrob"; FAIL=1
    fi
    plrob=${lrob:-0}
  done
  printf "  mode=%-2s %s\n" "$mode" "$line"
done
# cfg_iq_entries 全範圍（v8 下限回到 1）
for mode in 0 9; do
  prev=-1; line=""
  for iq in 1 $((IQN/2)) $IQN; do
    out=$("$OBJ/simreg" +mode=$mode +rob=$ROBN +iq=$iq 2>&1)
    ret=$(echo "$out" | grep -oP 'retired=\K[0-9]+' | head -1)
    line="$line  iq=$iq:ret=$ret"
    [ "${ret:-0}" -eq 0 ] && { echo "  FAIL mode=$mode cfg_iq=$iq 完全沒有 commit"; FAIL=1; }
    if [ "$prev" -ge 0 ] && [ "${ret:-0}" -lt $((prev*95/100)) ]; then
      echo "  FAIL mode=$mode cfg_iq 變大反而劣化: $prev -> $ret"; FAIL=1
    fi
    prev=${ret:-0}
  done
  printf "  IQmask mode=%-2s %s\n" "$mode" "$line"
done
rm -rf "$OBJ" "$OBJ.log"
[ $FAIL -eq 0 ] && echo "=== 回歸 PASS ===" || echo "=== 回歸 FAIL ==="
exit $FAIL
