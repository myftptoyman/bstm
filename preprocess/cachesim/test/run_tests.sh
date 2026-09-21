#!/usr/bin/env bash
# BSTM cachesim self-test.  Synthetic streams with analytically known hit rates.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SIM="$ROOT/cachesim"
B="$ROOT/build"
mkdir -p "$B"
PASS=0; FAIL=0

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  [ OK ] %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); printf '  [FAIL] %s\n' "$*"; }

# get <json> <dotted.path>
get() { python3 -c '
import json,sys
d=json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."):
    d=d[int(k)] if k.isdigit() else d[k]
print(d)' "$1" "$2"; }

# near <name> <json> <path> <expected> <tol>
near() {
  local v; v="$(get "$2" "$3")"
  if python3 -c "import sys; sys.exit(0 if abs($v-($4))<=($5) else 1)"; then
    ok "$1: $3 = $v (expect $4 +/- $5)"
  else
    bad "$1: $3 = $v (expect $4 +/- $5)"
  fi
}
# cmpop <name> <json> <path> <op> <bound>
cmpop() {
  local v; v="$(get "$2" "$3")"
  if python3 -c "import sys; sys.exit(0 if ($v $4 $5) else 1)"; then
    ok "$1: $3 = $v ($4 $5)"
  else
    bad "$1: $3 = $v (expected $4 $5)"
  fi
}

mkcfg() { # mkcfg <file> <l1d_size> <l2_size> <pf on/off> <inclusive true/false> [pf_index]
  cat > "$1" <<CFG
{ "line_size": 64, "page_size": 4096,
  "l1i": {"size": 32768, "ways": 8},
  "l1d": {"size": $2, "ways": 8},
  "l2":  {"size": $3, "ways": 8, "inclusive": $5},
  "l3":  {"size": 4194304, "ways": 16, "enabled": true},
  "itlb": {"entries": 32}, "dtlb": {"entries": 64},
  "prefetcher": {"enabled": $4, "entries": 64, "degree": 2, "distance": 1,
                 "min_conf": 2, "index": "${6:-page}"} }
CFG
}

say "=== BSTM cachesim self-test ==="
[ -x "$SIM" ] || { echo "cachesim not built"; exit 1; }

mkcfg "$B/base_nopf.json" 32768 524288 false true
mkcfg "$B/base_pf.json"   32768 524288 true  true pc

# ---------------------------------------------------------------- T1
# 8 B sequential loads, 64 B lines -> 8 accesses per line, 1 compulsory miss.
# Theory: L1D hit rate = 7/8 = 0.875, every L1D miss is compulsory so the
# L2 / L3 hit rate must be 0.
say "T1  sequential scan, 8 B stride (theory: L1D 0.875, L2 0.0)"
"$SIM" --gen seq 80000 8 8 > "$B/t1.mem.txt"
"$SIM" "$B/t1.mem.txt" "$B/base_nopf.json" "$B/t1.out" --quiet
near  T1 "$B/t1.out.stats.json" levels.l1d.hit_rate 0.875 0.0005
near  T1 "$B/t1.out.stats.json" levels.l2.hit_rate  0.0   0.0005
near  T1 "$B/t1.out.stats.json" levels.l3.hit_rate  0.0   0.0005
near  T1 "$B/t1.out.stats.json" levels.l1d.accesses 80000 0
n=$(stat -c %s "$B/t1.out"); [ "$n" = 80000 ] && ok "T1: overlay is 1 byte/access ($n)" \
                                              || bad "T1: overlay size $n != 80000"
# every access must be either an L1 hit (level 0) or a DRAM miss (level 3)
near  T1 "$B/t1.out.stats.json" overlay.level_histogram.l1   70000 0
near  T1 "$B/t1.out.stats.json" overlay.level_histogram.dram 10000 0

# ---------------------------------------------------------------- T2
# 16 KB working set (256 lines) re-read 20x in a 32 KB L1D: only the first
# pass misses.  Theory: hit rate = (5120-256)/5120 = 0.95
say "T2  16 KB loop x20 inside a 32 KB L1D (theory: 0.95)"
"$SIM" --gen loop 16384 20 64 8 > "$B/t2.mem.txt"
"$SIM" "$B/t2.mem.txt" "$B/base_nopf.json" "$B/t2.out" --quiet
near  T2 "$B/t2.out.stats.json" levels.l1d.hit_rate 0.95 0.0005

# ---------------------------------------------------------------- T3
# 256 KB working set: cyclic sweep of 4096 lines through a 512-line LRU L1D
# thrashes completely (0.0); it fits in the 8192-line L2 (theory 0.9 over 10
# iterations).
say "T3  256 KB loop x10 (theory: L1D 0.0 thrash, L2 0.9)"
"$SIM" --gen loop 262144 10 64 8 > "$B/t3.mem.txt"
"$SIM" "$B/t3.mem.txt" "$B/base_nopf.json" "$B/t3.out" --quiet
near  T3 "$B/t3.out.stats.json" levels.l1d.hit_rate 0.0 0.0005
near  T3 "$B/t3.out.stats.json" levels.l2.hit_rate  0.9 0.0005

# ---------------------------------------------------------------- T4
# Conflict: 32 KB/8-way/64 B L1D has 64 sets -> set stride 4096 B.
#   8 lines  -> exactly fills one set, 8 misses in 80 accesses -> 0.9
#   9 lines  -> classic LRU thrash, 0.0
say "T4  associativity conflict (theory: 8 lines 0.9, 9 lines 0.0)"
"$SIM" --gen conflict 8 4096 10 > "$B/t4a.mem.txt"
"$SIM" "$B/t4a.mem.txt" "$B/base_nopf.json" "$B/t4a.out" --quiet
near  T4a "$B/t4a.out.stats.json" levels.l1d.hit_rate 0.9 0.0005
"$SIM" --gen conflict 9 4096 10 > "$B/t4b.mem.txt"
"$SIM" "$B/t4b.mem.txt" "$B/base_nopf.json" "$B/t4b.out" --quiet
near  T4b "$B/t4b.out.stats.json" levels.l1d.hit_rate 0.0 0.0005
# those 9 lines land in 9 distinct L2 sets -> all but the cold misses hit L2
near  T4b "$B/t4b.out.stats.json" levels.l2.hit_rate 0.9 0.0005

# ---------------------------------------------------------------- T5
# Stride prefetcher: one access per line, constant 64 B stride, PC-indexed.
# Without the prefetcher every L1D miss goes to DRAM; with it the L2 should
# serve almost everything as a prefetch hit.
say "T5  stride prefetcher, 64 B stride"
"$SIM" --gen stridepf 20000 64 8 > "$B/t5.mem.txt"
"$SIM" "$B/t5.mem.txt" "$B/base_nopf.json" "$B/t5a.out" --quiet
"$SIM" "$B/t5.mem.txt" "$B/base_pf.json"   "$B/t5b.out" --quiet
near  T5off "$B/t5a.out.stats.json" levels.l2.hit_rate 0.0 0.0005
cmpop T5on  "$B/t5b.out.stats.json" levels.l2.hit_rate ">" 0.95
cmpop T5on  "$B/t5b.out.stats.json" prefetch.accuracy  ">" 0.90
cmpop T5on  "$B/t5b.out.stats.json" prefetch.coverage  ">" 0.90
cmpop T5on  "$B/t5b.out.stats.json" overlay.avg_unloaded_latency_cycles "<" \
      "$(get "$B/t5a.out.stats.json" overlay.avg_unloaded_latency_cycles)"

# ---------------------------------------------------------------- T6
# Random over 64 MB: L1D hit rate must be tiny, DTLB (64 entries, 4 KB pages)
# must miss almost always.
say "T6  random over 64 MB (theory: near-zero L1D hit rate)"
"$SIM" --gen random 200000 67108864 12345 > "$B/t6.mem.txt"
"$SIM" "$B/t6.mem.txt" "$B/base_nopf.json" "$B/t6.out" --quiet
cmpop T6 "$B/t6.out.stats.json" levels.l1d.hit_rate "<" 0.05
cmpop T6 "$B/t6.out.stats.json" tlb.dtlb.miss_rate  ">" 0.90

# ---------------------------------------------------------------- T7
# Multi-configuration single scan must be bit-identical to N separate runs.
say "T7  multi-config single scan == N independent runs"
cat "$B/t3.mem.txt" "$B/t6.mem.txt" "$B/t2.mem.txt" > "$B/t7.mem.txt"

cat > "$B/t7_sweep_l1d.json" <<CFG
{ "line_size": 64,
  "l1i": {"size": 32768, "ways": 8},
  "l1d": {"size": 32768, "ways": 8},
  "l2":  {"size": 524288, "ways": 8, "inclusive": false},
  "l3":  {"enabled": false},
  "prefetcher": {"enabled": false},
  "sweep": {"level": "l1d", "ways": 8,
            "sizes": [8192, 16384, 32768, 65536, 131072, 524288]} }
CFG
"$SIM" "$B/t7.mem.txt" "$B/t7_sweep_l1d.json" "$B/t7.out" --quiet
i=0
for sz in 8192 16384 32768 65536 131072 524288; do
  mkcfg "$B/t7_one_$sz.json" $sz 524288 false false
  "$SIM" "$B/t7.mem.txt" "$B/t7_one_$sz.json" "$B/t7_one_$sz.out" --quiet
  a=$(get "$B/t7.out.stats.json" "sweep.variants.$i.hits")
  b=$(get "$B/t7_one_$sz.out.stats.json" levels.l1d.hits)
  if [ "$a" = "$b" ]; then ok "T7 l1d $sz B: hits $a == $b"
  else bad "T7 l1d $sz B: sweep $a != single-config $b"; fi
  i=$((i+1))
done

cat > "$B/t7_sweep_l2.json" <<CFG
{ "line_size": 64,
  "l1i": {"size": 32768, "ways": 8},
  "l1d": {"size": 32768, "ways": 8},
  "l2":  {"size": 524288, "ways": 8, "inclusive": false},
  "l3":  {"enabled": false},
  "prefetcher": {"enabled": false},
  "sweep": {"level": "l2", "ways": 8,
            "sizes": [131072, 262144, 524288, 1048576, 2097152]} }
CFG
"$SIM" "$B/t7.mem.txt" "$B/t7_sweep_l2.json" "$B/t7l2.out" --quiet
i=0
for sz in 131072 262144 524288 1048576 2097152; do
  mkcfg "$B/t7_l2_$sz.json" 32768 $sz false false
  "$SIM" "$B/t7.mem.txt" "$B/t7_l2_$sz.json" "$B/t7_l2_$sz.out" --quiet
  a=$(get "$B/t7l2.out.stats.json" "sweep.variants.$i.hits")
  b=$(get "$B/t7_l2_$sz.out.stats.json" levels.l2.hits)
  if [ "$a" = "$b" ]; then ok "T7 l2 $sz B: hits $a == $b"
  else bad "T7 l2 $sz B: sweep $a != single-config $b"; fi
  i=$((i+1))
done

# ---------------------------------------------------------------- T8
# Overlay encoding must round-trip through the bstf.h macros.
say "T8  overlay byte encoding vs include/bstf.h"
python3 - "$B/t5b.out" "$B/t5b.out.stats.json" <<'PY'
import json,sys
data=open(sys.argv[1],'rb').read()
st=json.load(open(sys.argv[2]))
lvl=[0]*4; cls=[0]*16; pf=0; tlb=0
for x in data:
    lvl[x&3]+=1; cls[(x>>4)&0xF]+=1
    pf += (x>>2)&1; tlb += (x>>3)&1
h=st['overlay']['level_histogram']
assert lvl==[h['l1'],h['l2'],h['l3'],h['dram']], (lvl,h)
assert cls==st['overlay']['lat_class_histogram'], (cls,st['overlay']['lat_class_histogram'])
assert len(data)==st['accesses']['total']
assert pf==st['prefetch']['useful'], (pf,st['prefetch']['useful'])
print("  [ OK ] T8: decoded overlay matches stats (%d bytes, %d prefetch hits)"%(len(data),pf))
PY
[ $? -eq 0 ] && PASS=$((PASS+1)) || { FAIL=$((FAIL+1)); echo "  [FAIL] T8"; }

say ""
say "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
