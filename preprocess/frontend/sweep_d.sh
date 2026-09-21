#!/bin/sh
# ============================================================
# sweep_d.sh — wrongpath_depth (D) 敏感度掃描
#   用法: ./sweep_d.sh [trace] [D ...]
# 其他配置固定為 config.json，只改 D。輸出 markdown 表格。
# ============================================================
set -e
cd "$(dirname "$0")"
B=build
SIM=$B/frontend_sim
TRACE=${1:-$B/loop.trace}
[ $# -gt 1 ] && { shift; DS="$*"; } || DS="0 4 8 10 16 24 32"
[ -x "$SIM" ] || { echo "sweep_d: 請先 make"; exit 1; }
[ -f "$TRACE" ] || python3 gen_trace.py 400 20 > "$TRACE"

mkdir -p $B/sweep
for D in $DS; do
    sed "s/\"wrongpath_depth\": *[0-9]*/\"wrongpath_depth\": $D/" config.json > $B/sweep/d$D.json
    $SIM "$TRACE" $B/sweep/d$D.json $B/sweep/d$D.fe 2>/dev/null
done

python3 - $B $TRACE $DS <<'PY'
import json, os, sys
B, trace = sys.argv[1], sys.argv[2]
DS = [int(x) for x in sys.argv[3:]]
rows = []
for D in DS:
    st = json.load(open(os.path.join(B, "sweep", "d%d.stats.json" % D)))
    rows.append((D, st))
base = rows[0][1]
print("trace: %s   (%d correct-path blocks, %d instructions)" %
      (trace, base["correct_path_blocks"], base["instructions"]))
print()
print("|  D | mispredicts | mispred rate | vs D=0 | MPKI | cond dir mispred | uBTB hit | BTB hit | BTB override | avg bubbles | wp blocks |")
print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
b0 = base["mispredicts"]
for D, st in rows:
    print("| %2d | %d | %.4f%% | %+.1f%% | %.3f | %.4f%% | %.4f%% | %.4f%% | %.4f%% | %.4f | %d |" % (
        D, st["mispredicts"], 100 * st["mispredict_rate"],
        100.0 * (st["mispredicts"] - b0) / b0 if b0 else 0.0,
        st["mpki"],
        100 * st["cond_dir_mispredict_rate"],
        100 * st["ubtb_hit_rate"], 100 * st["btb_hit_rate"],
        100 * st["btb_override_rate"], st["avg_bubbles_per_block"],
        st["wrongpath_blocks"]))
PY
