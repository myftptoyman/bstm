#!/bin/bash
# BSTM 契約驗收 — 監督者用。把 CONTRACT.md 的規則變成機械檢查。
# 用法: ci/check_contract.sh [module.v ...]   不給參數則檢查全部 model/ooo
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
PASS=0; FAIL=0
red(){ printf '\033[31m%s\033[0m\n' "$*"; }
grn(){ printf '\033[32m%s\033[0m\n' "$*"; }
ylw(){ printf '\033[33m%s\033[0m\n' "$*"; }
chk(){ if [ "$1" = 0 ]; then grn "  PASS  $2"; PASS=$((PASS+1)); else red "  FAIL  $2"; FAIL=$((FAIL+1)); fi; }

FILES=${@:-$(find model/ooo -name '*.v' ! -name 'top.v' | sort)}

echo "=============================================================="
echo " BSTM 契約驗收"
echo "=============================================================="

# ---------- 規則 5：禁止的語法 ----------
echo; ylw "[規則 5] 禁止 \$display / \$finish / initial / DPI"
for f in $FILES; do
  # 先剝掉 // 與 /* */ 註解再檢查
  bad=$(sed -E 's,/\*.*\*/,,g; s,//.*,,' "$f" | grep -nE '\$(display|write|finish|stop|fatal|error|monitor)|^[[:space:]]*initial[[:space:]]|import "DPI' | head -5)
  if [ -n "$bad" ]; then red "  FAIL  $f"; echo "$bad" | sed 's/^/          /'; FAIL=$((FAIL+1));
  else grn "  PASS  $f"; PASS=$((PASS+1)); fi
done

# ---------- 規則 4：case 必須有 default ----------
echo; ylw "[規則 4] 每個 case 都要有 default"
for f in $FILES; do
  nc=$(grep -cE '^\s*(unique\s+)?case[xz]?\s*\(' "$f")
  nd=$(grep -cE '^\s*default\s*:' "$f")
  if [ "$nc" -gt "$nd" ]; then red "  FAIL  $f  (case=$nc default=$nd)"; FAIL=$((FAIL+1));
  else grn "  PASS  $f  (case=$nc default=$nd)"; PASS=$((PASS+1)); fi
done

# ---------- 規則 1：禁止寬算術（48-bit counter 除外）----------
echo; ylw "[規則 1] 禁止寬欄位宣告 (>8 bit，counter/bundle 除外)"
for f in $FILES; do
  bad=$(grep -nE '^[[:space:]]*(reg|wire)[[:space:]]*\[[[:space:]]*[0-9]+[[:space:]]*:[[:space:]]*0[[:space:]]*\]' "$f" \
        | sed -E 's/.*\[[[:space:]]*([0-9]+)[[:space:]]*:.*/\1 &/' \
        | awk '{w=$1+1; if (w>8 && w!=48 && w!=32 && w!=64 && w!=128) {$1=""; print}}' | head -5)
  if [ -n "$bad" ]; then ylw "  WARN  $f  寬欄位（請人工確認是 bundle 或 counter）"; echo "$bad" | sed 's/^/          /';
  else grn "  PASS  $f"; PASS=$((PASS+1)); fi
done

# ---------- Yosys：可合成 + 無非同步記憶體 + 無 latch ----------
echo; ylw "[Yosys] 合成 / \$__RAMGEM_ASYNC_ / latch / 組合迴路"
command -v docker >/dev/null || { red "  docker 不可用，略過"; }
for f in $FILES; do
  top=$(basename "$f" .v)
  out=$(docker run --rm -v "$ROOT/model/ooo":/w -w /w hdlc/yosys:latest yosys -p \
        "read_verilog -I. ${f#model/ooo/}; hierarchy -check -top $top; proc; opt; check -assert;
         select -assert-none t:\$dlatch t:\$_DLATCH_* t:\$adlatch t:\$shift t:\$shiftx t:\$mem t:\$memrd t:\$memwr;
         stat" 2>&1)
  rc=$?
  if [ $rc -ne 0 ]; then red "  FAIL  $top  yosys 失敗"; echo "$out" | grep -iE 'error|warning' | head -5 | sed 's/^/          /'; FAIL=$((FAIL+1)); continue; fi
  # yosys 的 select -assert-none 失敗會讓整個腳本非零退出並印 ERROR；
  # 不可以 grep cell 名稱 —— select 指令文字會被 log 回顯而自己匹配到自己。
  if echo "$out" | grep -q "^ERROR"; then
    red "  FAIL  $top  斷言失敗"; echo "$out" | grep "^ERROR" | head -2 | sed 's/^/          /'; FAIL=$((FAIL+1)); continue
  fi
  cells=$(echo "$out" | grep 'Number of cells:' | tail -1 | awk '{print $NF}'); [ -z "$cells" ] && cells=0
  grn "  PASS  $top  cells=$cells"; PASS=$((PASS+1))
done

# ---------- Verilator lint ----------
echo; ylw "[Verilator] lint-only -Wall"
if command -v verilator >/dev/null; then
  for f in $FILES; do
    top=$(basename "$f" .v)
    lg=$(cd model/ooo && verilator --lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSED -I. --top-module "$top" "${f#model/ooo/}" 2>&1)
    err=$(echo "$lg" | grep '%Error' | grep -vc 'Exiting due to')
    und=$(echo "$lg" | grep -c 'UNDRIVEN')
    [ "$und" -gt 0 ] && ylw "        (UNDRIVEN x$und — stub 未填 body 時正常)" 
    if [ "$err" -gt 0 ]; then red "  FAIL  $top  ($err errors)"; FAIL=$((FAIL+1));
    else grn "  PASS  $top"; PASS=$((PASS+1)); fi
  done
else red "  verilator 不可用"; fi

# ---------- 檔案所有權 ----------
echo; ylw "[所有權] agent 是否越界寫入"
for prot in include/bstf.h model/ooo/common/ifc.vh model/ooo/top.v CONTRACT.md; do
  if [ -f "$prot" ]; then
    m=$(stat -c %Y "$prot")
    grn "  INFO  $prot  mtime=$(date -d @$m '+%m-%d %H:%M')"
  fi
done

echo; echo "=============================================================="
echo " PASS=$PASS  FAIL=$FAIL"
echo "=============================================================="
[ "$FAIL" -eq 0 ]
