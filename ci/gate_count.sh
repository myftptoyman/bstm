#!/bin/bash
# Agent F 的發現：proc;opt 的 cell 數不能當 bitwise op 的代理
#   —— 那階段的 $mux 是 word-level 且大量冗餘。
# gate-level（synth -flatten; abc -g AND,OR,XOR,NAND,NOR,XNOR）才是真數字。
# 這支腳本產生 CI 之後要用的正式度量。
cd "$(dirname "$0")/../model/ooo" || exit 1
SRC="top.v frontend/fe_front.v rename/rn_rename.v backend/be_dispatch.v backend/be_iq.v backend/be_eu.v backend/be_rob.v lsu/lsu_q.v"
for top in "$@"; do
  out=$(docker run --rm -v "$PWD":/w -w /w hdlc/yosys:latest yosys -p "
    read_verilog -I. $SRC
    hierarchy -top $top
    synth -flatten -top $top
    abc -g AND,OR,XOR,NAND,NOR,XNOR
    opt_clean
    stat" 2>&1)
  g=$(echo "$out" | grep -cE '^\s+\$_(AND|OR|XOR|NAND|NOR|XNOR|NOT)_' )
  cells=$(echo "$out" | grep 'Number of cells:' | tail -1 | awk '{print $NF}')
  ff=$(echo "$out" | grep -E '\$_(SDFF|DFF)' | awk '{s+=$2} END{print s+0}')
  gates=$(echo "$out" | grep -E '^\s+\$_(AND|OR|XOR|NAND|NOR|XNOR|NOT)_' | awk '{s+=$2} END{print s+0}')
  printf "  %-14s gates=%-8s flops=%-6s total_cells=%s\n" "$top" "$gates" "$ff" "$cells"
done
