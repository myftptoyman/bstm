#!/bin/bash
# ============================================================
# run_coremark.sh -- drive Spike over CoreMark and convert its commit log
#                    into the BSTF trace triple.  Agent H, BSTM project.
#
#   ./run_coremark.sh --self-test          200k instructions, quick sanity
#   ./run_coremark.sh --full               50M-instruction steady-state trace
#   ./run_coremark.sh --small              1M-instruction trace for other agents
#   ./run_coremark.sh --count N --name X   anything else
#
# Nothing outside preprocess/spike/ and traces/ is written.
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BSTM="$(cd "$HERE/../.." && pwd)"
TRACES="$BSTM/traces"

SPIKE_DIR=${SPIKE_DIR:-/home/toyman/work/andes-spike/build}
SPIKE=$SPIKE_DIR/spike
ELF=${ELF:-/home/toyman/work/mc_run_cmv5/coremark_v5.exe}

# --- Spike configuration for the Andes bare-metal CoreMark image ---------
#  * ISA from the ELF's Tag_RISCV_arch (rv64imafdc + Zba/Zbb/Zbc/Zbs + Xandes)
#  * marchid copied from mc_run_cmv5/iss.cfg (Andes Spike refuses marchid 0)
#  * reset vector moved out of the way: the image is linked at 0x0, which
#    collides with Spike's default boot ROM at 0x80
#  * memory map: .text/.text.init @0, tohost @0x100000, .data/.stack
#    @0x70000000, .data.io @0x80000000
ISA=${ISA:-rv64imafdc_zicntr_zihpm_zba_zbb_zbc_zbs_xandes}
ISA_TAG=${ISA_TAG:-rv64gc_zba_zbb_zbc_zbs_xandes}
MARCHID=${MARCHID:-0x8000000000008a66}
MEM=${MEM:-0x0:0x200000,0x70000000:0x100000,0x80000000:0x10000}
RSTVEC=${RSTVEC:-0x60000000}

# CoreMark's timed region begins at the first call to start_time().
START_PC=${START_PC:-0xf4}          # nm coremark_v5.exe | grep start_time
STOP_PC=${STOP_PC:-0x90}            # _pass: the final `beqz zero, _pass` spin

FETCH_WIDTH=${FETCH_WIDTH:-4}
LINE=${LINE:-64}

COUNT=""
NAME=""
ITERS=""
OUTDIR="$TRACES"

# One CoreMark iteration is ~237k instructions on this build, and the shipped
# ELF is built for 4 iterations (951,300 steady-state instructions).  To reach
# 50M we bump CoreMark's iteration count, which lives in the `seed4_volatile`
# initialiser -- patched into a private copy of the ELF, never the original.
while [ $# -gt 0 ]; do
  case "$1" in
    --self-test) COUNT=200000;    NAME=selftest;     ITERS=4;   OUTDIR="$HERE/selftest"; shift ;;
    --full)      COUNT=50000000;  NAME=coremark;     ITERS=256; shift ;;
    --small)     COUNT=1000000;   NAME=coremark_1m;  ITERS=256; shift ;;
    --whole)     COUNT=0;         NAME=coremark_whole; ITERS=4; shift ;;
    --count)     COUNT="$2"; shift 2 ;;
    --name)      NAME="$2";  shift 2 ;;
    --iters)     ITERS="$2"; shift 2 ;;
    --outdir)    OUTDIR="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$COUNT" ] || { echo "nothing to do; try --self-test / --full / --small" >&2; exit 2; }
[ -n "$NAME" ]  || NAME="coremark_${COUNT}"

[ -x "$SPIKE" ] || { echo "spike not found at $SPIKE" >&2; exit 1; }
[ -r "$ELF" ]   || { echo "ELF not found at $ELF" >&2; exit 1; }
mkdir -p "$OUTDIR"
make -s -C "$HERE" bstf_gen bstf_check

# --- private, iteration-patched copy of the ELF (original is never touched) ---
if [ -n "${ITERS:-}" ] && [ "$ITERS" != 4 ]; then
  mkdir -p "$HERE/elf"
  PATCHED="$HERE/elf/coremark_v5_i${ITERS}.exe"
  if [ ! -f "$PATCHED" ]; then
    "$HERE/patch_iterations.py" "$ELF" "$PATCHED" --iterations "$ITERS"
  fi
  ELF="$PATCHED"
fi

SHA=$(sha256sum "$ELF" | cut -d' ' -f1)
SPIKE_CMD="$SPIKE --isa=$ISA --csr-marchid $MARCHID --reset-vector=$RSTVEC -m$MEM --pc=0x0 -l --log-commits $ELF"

# Wrong-path shadow needs Agent A's .fe overlay for the *same* trace; if it is
# not there yet, run frontend_sim first and re-run this script.
FE_ARG=()
if [ -r "$OUTDIR/$NAME.fe" ]; then
  FE_ARG=(--fe "$OUTDIR/$NAME.fe" --shadow-k "${SHADOW_K:-40}" --shadow-blocks "${SHADOW_D:-10}")
  echo "    shadow: using $OUTDIR/$NAME.fe  (K=${SHADOW_K:-40}, D=${SHADOW_D:-10})"
else
  echo "    shadow: $OUTDIR/$NAME.fe not present -> no wrong-path shadow this pass"
fi

echo "=== spike -> bstf_gen: $NAME ($COUNT instructions) ==="
echo "    elf   : $ELF"
echo "    sha256: $SHA"
echo "    spike : $SPIKE_CMD"

export LD_LIBRARY_PATH="$SPIKE_DIR:${LD_LIBRARY_PATH:-}"
set +e
"$SPIKE" --isa="$ISA" --csr-marchid "$MARCHID" --reset-vector="$RSTVEC" \
         -m"$MEM" --pc=0x0 -l --log-commits --log=/dev/stdout "$ELF" 2>/dev/null \
| "$HERE/bstf_gen" \
    --out "$OUTDIR/$NAME" \
    --workload coremark \
    --isa "$ISA_TAG" \
    --start-pc "$START_PC" \
    --stop-pc  "$STOP_PC" \
    --count    "$COUNT" \
    --fetch-width "$FETCH_WIDTH" \
    --line "$LINE" \
    --audit 100 \
    "${FE_ARG[@]}" \
    --elf "$ELF" --elf-sha256 "$SHA" \
    --spike-cmd "$SPIKE_CMD" \
    --note "CoreMark steady state: trace starts at the first retire of start_time() (PC $START_PC)"
rc=${PIPESTATUS[1]}
set -e
[ "$rc" = 0 ] || { echo "bstf_gen failed ($rc)" >&2; exit "$rc"; }

echo
"$HERE/bstf_check" "$OUTDIR/$NAME.bstf" \
    --fe   "$OUTDIR/$NAME.frontend.txt" \
    --mem  "$OUTDIR/$NAME.mem.txt" \
    --imem "$OUTDIR/$NAME.imem.txt" \
    $([ -r "$OUTDIR/$NAME.mem.wp.txt" ] && echo --memwp "$OUTDIR/$NAME.mem.wp.txt")
