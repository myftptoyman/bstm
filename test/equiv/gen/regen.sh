#!/bin/sh
# 重新產生 gen/chain_bs.c（需要 yosys；本機目前沒有，所以 repo 裡放的是
# 從 prototype/cbench.c 抽出來的同一份產生物）。
# 之後 Agent C 的 tools/bstm-cc 完成後，把 gen.py 換成 bstm-cc 即可。
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$HERE/../../..
: "${YOSYS:=yosys}"
: "${TOPMOD:=chain}"
command -v "$YOSYS" >/dev/null || { echo "yosys not found; 保留現有 gen/chain_bs.c"; exit 0; }
$YOSYS -p "read_verilog $TOP/prototype/chain.v $TOP/prototype/stage.v; \
           hierarchy -top $TOPMOD; proc; opt_expr; opt_clean; pmuxtree; \
           flatten; opt -fast; write_json $HERE/chain.json"
python3 "$TOP/prototype/gen.py" "$HERE/chain.json" "$TOPMOD" > "$HERE/chain_raw.c"
python3 - "$HERE" <<'PY'
import sys, re
here = sys.argv[1]
src = open(here + "/chain_raw.c").read()
src = src.replace('#include <stdint.h>\n','')
src = src.replace('typedef uint64_t vec_t;        /* 64 lanes = 64 instances */\n','')
src = src.replace('#define VZERO ((vec_t)0)\n','').replace('#define VONES (~(vec_t)0)\n','')
src = src.replace('typedef struct {','struct chain_state_ {',1).replace('} state_t;','};',1)
src = src.replace('static void eval_cycle(','void chain_eval_cycle(')
src = src.replace('state_t *s, const state_t *p','chain_state_t *s, const chain_state_t *p')
open(here + "/chain_bs.c","w").write(
    '/* 產生物：gen/regen.sh (yosys + prototype/gen.py)。勿手改。 */\n'
    '#include "chain_bs.h"\n' + src +
    '\nsize_t chain_state_size(void) { return sizeof(chain_state_t); }\n')
PY
rm -f "$HERE/chain_raw.c"
echo "regenerated $HERE/chain_bs.c"
