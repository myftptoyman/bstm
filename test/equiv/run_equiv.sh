#!/bin/sh
# test/equiv — 一鍵對拍（PLAN §9.2 第一層驗證）
# 用法: ./run_equiv.sh [每階段 cycle 數]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TESTDIR=$HERE/..
N=${1:-500000}

make -s -C "$HERE"
make -s -C "$TESTDIR" traces/synth0.bstf traces/synth1.bstf

cd "$TESTDIR"
echo "=============================================================="
echo " BSTM equivalence check   Verilator (--x-assign 0 --x-initial 0)"
echo "                          vs bit-sliced lane 0      N=$N"
echo "=============================================================="
rc=0
echo "--- 1) prototype/chain.v (830 cell, 32-stage) ---"
./equiv/build/equiv_chain "$N" traces/synth0.bstf || rc=1
echo
echo "--- 2) test/mock/mock_ooo_top.v (ooo_top 埠列 + 11 個 48-bit counter) ---"
./equiv/build/equiv_mock "$N" traces/synth0 || rc=1
echo
if [ $rc = 0 ]; then echo "EQUIV PASS"; else echo "EQUIV FAIL"; fi
exit $rc
