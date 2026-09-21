# BSTM — Bit-Sliced Timing Model

A high-throughput microarchitecture design-space-exploration platform: **write your
pipeline timing model in Verilog, compile it to bit-sliced C with Yosys, and simulate
thousands of independent (SimPoint × config) instances at once.** Instruction semantics
come from an ISS (Spike) offline, so the model contains only structure — no datapath,
no ALU, no register values.

> **Status:** research prototype. End-to-end pipeline works and is bit-exact against
> Verilator. Absolute cycle accuracy is **not** validated — see [Honest limitations](#honest-limitations).

---

## The idea in one paragraph

A single CPU pipeline has almost no spatial parallelism — ten-odd structurally different
stages, so splitting one simulation across threads buys you nothing. But design-space
exploration has a different axis that is embarrassingly parallel and that nobody exploits:
you are running the *same model* over `N` SimPoint intervals × `M` configurations. BSTM
packs 64 of those instances into the bits of a machine word, so one `AND` instruction
evaluates one gate for 64 instances at once. This is the 1985 ATPG technique
(parallel-pattern simulation, PPSFP) applied to performance modelling.

The catch, measured rather than assumed: **per target cycle BSTM is 1.8×–5.5× *slower*
than Verilator.** The win is entirely the 64-lane multiplier divided by the
bit-serialisation tax. That is 12–35× net, not 64×.

## Architecture

```
┌── Offline (once per workload) ──────────────────────────────────────┐
│  Spike ISS ──► instruction stream ──► .bstf   (16 B/instruction)    │
│           ├──► branch stream ──► TAGE/uBTB/BTB sim ──► .fe (1 B/blk)│
│           └──► address stream ──► L1/L2/L3 + TLB sim ──► .mem (1 B) │
└─────────────────────────────────────────────────────────────────────┘
                                 │  flat, read-only, shared by all lanes
┌── Compile (once per model) ────┼────────────────────────────────────┐
│  model.v ──► Yosys write_json ─┼─► bstm-cc ──► bit-sliced C         │
│      └─────► Verilator ────────┼─► golden reference (1 instance)    │
└─────────────────────────────────────────────────────────────────────┘
                                 │
┌── Runtime ──────────────────────────────────────────────────────────┐
│  scalar refill (gather + 64×64 bit transpose)                       │
│            ↓                                                        │
│  fetch buffer  ◄── the scalar/vector boundary                       │
│            ↓                                                        │
│  timing model, 64 lanes in lockstep, pure bitwise, zero branches    │
│            ↓                                                        │
│  per-instance counters ──► CSV                                      │
└─────────────────────────────────────────────────────────────────────┘
```

Three design rules make this work, and violating any of them collapses it:

1. **Fields ≤ 8 bit, no wide arithmetic.** Bit-slicing is free for 1-bit logic; a
   `W`-bit add costs ~5W operations. PC, addresses and data never enter the model.
2. **Max-size structures + runtime mask.** All 64 lanes execute the same code, so
   configuration differences must be *values*, not *netlists*.
3. **No asynchronous memory.** Everything is flops + mux chains. The integrated
   netlist has `Number of memories: 0`.

## What is actually built

| Component | What it does |
|---|---|
| `model/ooo/` | 4-issue out-of-order RV64 timing model, 10 stages, 7 Verilog modules |
| `preprocess/spike/` | Spike → `.bstf` writer, with a full-stream decoder cross-check |
| `preprocess/frontend/` | TAGE + 16-entry uBTB + 8K 4-way L2 BTB + 16-entry RAS |
| `preprocess/cachesim/` | L1I/L1D 32 KB, L2 512 KB, L3 4 MB, TLBs, stride prefetcher |
| `tools/bstm-cc/` | Yosys JSON → bit-sliced C compiler |
| `lib/runtime/` | mmap reader, AVX2 bit-transpose, refill, thread pool, statistics |
| `test/equiv/` | Verilator ↔ bit-sliced cycle-by-cycle equivalence harness |
| `ci/` | Contract checker: enforces the three design rules mechanically |

Modelled structures (max sizes, runtime-maskable): ROB 64, IQ 32, LDQ/STQ 16, MSHR 8,
PRF 64, fetch/issue/commit width 4.

## Measured results

All numbers on an Intel i7-12700, single core unless stated, gcc `-O3 -march=native`.

**Bit-slicing vs Verilator** (830-cell design, the measurement that justifies the whole approach):

| | Verilator (1 inst) | BSTM per target-cycle | BSTM 64 lanes | net |
|---|---|---|---|---|
| 4-bit fields | 5.99 M/s | 3.24 M/s | **208 M/s** | **35×** |
| 16-bit fields | 5.91 M/s | 1.07 M/s | 69 M/s | 12× |

Verilator is insensitive to field width (5.99 → 5.91); BSTM loses 3×. This is why
rule 1 exists.

**Integrated model** — 4-issue OOO, 10 stages, ROB 64 / IQ 32 / LDQ-STQ 16 / MSHR 8 / PRF 64:

```
ooo_top:  38,549 cells   0 memories   0 latches   0 variable shifts
          139,924 nets  →  3,432 slots   (40.8x liveness compression)
          141,317 bitwise ops per target cycle
          working set:  27 KB slots + 77 KB state = 104 KB   (target was < 512 KB)
```

Verified **bit-exact against Verilator on the real 38,549-cell model**, cycle by cycle,
every output bit. The equivalence harness also randomly asserts reset, so the reset
path is always exercised.

**Backend comparison** — four ways to turn 141,317 ops/cycle into something runnable:

| backend | compile | target-cycles/s | ops/s |
|---|---|---|---|
| one big function + gcc -O1 | **OOM after 10 min (27 GB)** | — | — |
| bytecode interpreter | 0.02 s | 4.8 K | 0.68 G |
| hand-emitted x86-64 JIT | **0.003 s** | 11.8 K | 1.66 G |
| **36 split functions + gcc -O2** | 4 min 47 s | **36.7 K** | **5.19 G** |

Three things this measurement settles:

1. **Emitting one large C function does not scale.** Splitting into functions — the same
   trick Verilator uses for `--output-split` — is mandatory, not an optimisation.
2. **"There is no register allocation left to do" is wrong.** The hand-written JIT does
   load-load-op-store for every op (28 bytes/op); gcc wins 3.1x by keeping local chains
   in registers.
3. **A JIT's value here is compile time, not speed** — 0.003 s versus 287 s, a factor of
   95,000. Sweeping 100 structural configurations costs 8 hours with gcc and 0.3 s with
   a JIT. Both backends have a place.

For anyone building this properly: CIRCT's **arcilator** already compiles hardware
dialects to LLVM IR and JITs them, with an `arc` dialect designed for simulation. It does
not bit-slice, but that is an `i1`→`i64` type conversion pass in MLIR — a far better
starting point than hand-rolling a JIT.

**A bottleneck the design document did not anticipate:** once liveness compresses the
*data* working set to 27 KB, the *instruction* footprint takes over. 141,317 ops is
roughly 800 KB of machine code against a 32 KB L1i. A 20,015-cell design measures ~1.7x
slower than linear extrapolation predicts. This is now the top open problem.

Gate-level (the honest metric — `proc; opt` cell counts are misleading by up to 12×):

| module | proc/opt cells | gate-level gates | flops |
|---|---:|---:|---:|
| `be_iq` | 7,852 | **81,737** | 4,688 |
| `lsu_q` | 12,743 | 23,075 | 1,556 |
| `rn_rename` | 4,761 | 14,531 | 546 |

(An earlier revision of this file reported these gate counts at exactly twice their real
value: `ci/gate_count.sh` summed both of the `=== module ===` blocks yosys emits — one from
`synth`, one from the final `stat`. The module *ranking* was unaffected, which is why the
error survived review. Three independent measurements now agree on the flop counts.)

**Equivalence checking** — the safety net, and it is green:

```
chain.v         421,200 cycles   1,263,600 signal-compares   0 mismatches
mock_ooo_top    414,142 cycles   5,797,988 signal-compares   0 mismatches
  ... on a real CoreMark trace                               0 mismatches
```

**Runtime primitives**

| | naive | optimised |
|---|---|---|
| 64×64 bit transpose | 228 ns | **45.8 ns** (AVX2) |
| `unpack_lanes(3 bit)` | 98 ns | **12.6 ns** (BMI2 `pdep`) |

**End-to-end IPC** — the real model, the real CoreMark trace, via Verilator:

```
IPC 1.381     (ROB occupancy 28.5 / 64)

stall breakdown        cycles      share
  rename              254,365      51 %     <- physical register file exhausted
  lsq                 112,212      22 %
  iq                   82,639      17 %
  fetch                54,584      11 %
  rob                     141    0.03 %     <- ROB almost never full
  mshr                      0       0 %     <- CoreMark fits in L1D
```

**This number is an upper bound**, and knowing why matters more than the number: wrong-path
shadow expansion is not implemented yet (`shadow_off` is 0 everywhere), so no misprediction
penalty is ever paid. With a 4.93 % block mispredict rate and a ~12-cycle penalty, the true
figure is likely nearer 1.0–1.1.

### The physical register sweep, and why it mattered

The stall table above looks like it says "rename is the bottleneck, the PRF is too small."
That reading is wrong, and the sweep that tested it is the most useful thing this model has
produced so far:

| PRF | IPC | rename stall<br>(freelist) | ROB stall | ROB occupancy |
|---|---|---:|---:|---:|
| 64 | 1.381 | 61,457 | 141 | 28.5 |
| 96 | 1.391 | 2,936 | 37,509 | 34.1 |
| 128 | 1.391 | **0** | 40,445 | 34.1 |
| 256 | 1.391 | **0** | 40,445 | 34.1 |

**Quadrupling the physical register file moves IPC by 0.7 %.** Everything from 96 upwards is
bit-identical, which was predicted from first principles before the sweep ran: live physical
registers are bounded by 32 committed architectural mappings plus at most `ROB_N` = 64
in-flight destinations, so 96 saturates by construction.

The stall attribution was the problem, not the register file:

```verilog
// rn_rename.v
wire c_rn_inc = (|de_valid) & (~rn_ready | ~enough_free) & ~flush;
                               ^^^^^^^^^^ downstream backpressure
// be_dispatch.v
wire blocked = rob_full | lsq_full | mshr_full | (~ds_ready);
assign rn_ready = ~blocked & ~flush;
```

A full IQ, LSQ, ROB or MSHR all lower `rn_ready`, and every one of those cycles was booked
against rename — double-counted with the downstream counters that already recorded them.
With the attribution fixed, freelist pressure reaches exactly zero at PRF 128, matching the
first-principles bound. The earlier "stall stays at 245,580 regardless of PRF" reading was
entirely an artefact of the counter.

What the corrected sweep actually shows is a textbook Amdahl result: removing the rename
bottleneck pushes pressure downstream, ROB stalls go from 141 cycles to 40,445 and ROB
occupancy from 28.5 to 34.1, and IPC barely moves because the LSQ and IQ are waiting behind
it. **A miscounted stall does not merely give the wrong number — it hides the trend, which
is the thing design-space exploration exists to find.**

Chasing it further turned up two more measurement problems, neither of which is a bug in the
model:

- **The counters are not defined symmetrically.** `cnt_st_lsq` only fires when a uop is
  genuinely blocked; `cnt_st_iq` fires whenever `ds_ready` is low regardless of whether
  anything wanted to issue. The inflated counter is still the smaller one, so real IQ
  pressure is lower than 17 %.
- **`lsq_full` is a threshold artefact.** By Little's Law the LSQ holds about 8 of its 32
  entries on average (0.387 memory uops/cycle × 20.7 cycles of ROB residency), yet it reports
  full 22 % of the time. The dispatch protocol is all-or-nothing — "fewer than 4 free slots"
  counts as full. Adding LDQ/STQ entries would achieve nothing; the protocol is what needs
  to change.

### The ROB sweep, and a bug that only a sweep could find

| `cfg_rob_entries` | IPC | ROB stall | ROB occupancy |
|---|---|---:|---:|
| 64 | 1.439 | 5,769 | 28.7 |
| 96 | 1.443 | 982 | 29.3 |
| 128 | 1.443 | 12 | 29.2 |

Doubling the ROB buys 0.3 %. Like the register file, it is not the bottleneck — average
occupancy is 29 of 128 entries, and the LSQ and IQ are queued up behind it.

Getting to those numbers required fixing a failure mode specific to the max-size-plus-mask
pattern. Raising `cfg_rob_entries` past 64 collapsed IPC from 1.391 to 0.072. The cause was
not a width mismatch — it was a *capacity* shortfall in `be_eu`, whose completion wheel was
4 lanes × 16 slots. When `lsu_done` wins a writeback port the losing entry is rescheduled to
a later free slot; a larger ROB means more in-flight uops, more contention, and eventually
no free slot, at which point the entry was **silently dropped**. Four drops in 500,000
cycles were enough, because each one wedges a ROB head permanently.

Three things about this are worth generalising:

- **Static checking cannot see it.** `[6:0]` assigned to `[6:0]` is legal; the problem is
  that 7 bits cannot hold the value 128. Lint and synthesis both pass.
- **Testing the default configuration cannot see it.** Built at `ROB_N` = 128 but masked to
  64, the model is bit-identical to a 64-entry build and every test passes. The path above
  64 had never been executed.
- **Filling a structure is not the same as filling it under contention.** A workload that
  pushed 64 uops in flight did not trigger it; one that also contended for the writeback
  port did.

The fix replaced the wheel with a pool addressed by `robidx`, so that each in-flight uop
owns its slot by construction — no allocation, no free-slot search, no path that can drop.
State went *down* by 165 bits and IPC at the original 64-entry setting improved 3.5 %,
because the reschedule path had been costing performance even when it wasn't dropping.

**A sweep-design consequence worth knowing:** because the modelling rules require max-size
structures with a runtime mask, `cfg_iq_entries = 48` costs exactly what 64 costs. IQ is
really a choice between 32 and 64, LDQ/STQ between 16 and 32, and PRF 96 sits at the same
cost point as 128. Intermediate values have IPC meaning but no cost meaning, so a
cost/benefit curve has to be plotted against quantised cost points rather than configuration
values.

**CoreMark trace** (real Spike, 50 M instructions from steady state):

```
ALU 46.52 %  LOAD 22.77 %  BRANCH 21.21 %  STORE 4.40 %  MUL 3.96 %
memory ops 27.17 %   fetch-block average 3.19 instructions
```

Validated by decoding all 50 M instructions with an independent decoder and comparing
against Spike's commit log: 0 mismatches on encodings, memory presence, memory size,
registers and branch targets. Total retired instruction count agrees with a pre-existing
Andes RTL co-simulation log to within the epilogue (961,438 vs 961,432).

**Front-end predictor**, on the real 50 M-instruction CoreMark trace:

| | value |
|---|---|
| L2 BTB (FTB) hit rate | **99.9941 %** |
| conditional direction mispredict | 5.919 % |
| MPKI | 12.52 |
| uBTB hit rate (16 entry) | 54.04 % |
| RAS accuracy (49,190 returns) | 98.87 % |
| average bubbles / fetch block | 0.4447 |

The direction mispredict rate is **at the information-theoretic limit for its predictor
class**, not a tuning failure. Measured against unbounded, alias-free ideal predictors on
the same trace:

| predictor | 1 M window |
|---|---|
| perfect bimodal (one 2-bit counter per PC) | 10.393 % |
| ideal global history H=64 (perfect `(PC, history)` table) | 7.164 % |
| **this simulator's TAGE (4×1K, hist 8–64)** | **7.165 %** |

TAGE matches the ideal H=64 bound to three decimal places, and the bimodal control
matches ideal bimodal bit-for-bit (10.666 % vs 10.663 %), which rules out implementation
error. Scaling TAGE 16× only reaches 5.73 %, so this is not a capacity limit either.
Going lower requires local history or a statistical corrector (TAGE-SC-L class) — a
specification change, not a bug fix.

**A concrete DSE result** — the kind of answer the platform exists to produce:

| uBTB entries | hit rate | avg bubbles/block | mispredict rate |
|---|---|---|---|
| 16 | 54.04 % | 0.4447 | unchanged |
| 64 | 93.22 % | **0.2193** | unchanged |

Front-end bubbles are limited by uBTB *capacity*, not by prediction *accuracy*. Growing
the uBTB halves the bubble rate while leaving the mispredict rate bit-identical.

## A result worth reading: the wrong-path depth cliff

Functional-first simulation cannot observe wrong-path execution directly; you pre-expand
`D` fetch blocks down the mispredicted path. How sensitive are the results to `D`?

Under the conservative default (`wrongpath_train_tage: 0` — TAGE counters update at
retire, and wrong-path instructions never retire), **mispredict rate is bit-identical
for all `D` from 0 to 32**. The only surviving sensitivity is front-end bubbles, and it
is *non-monotonic*:

| D | 4 | 8 | 12 | 14 | 15 | **16** | 17 | 24 | 32 |
|---|---|---|---|---|---|---|---|---|---|
| uBTB hit (U=16) | 7.0 % | 16.3 % | 25.6 % | 30.2 % | 32.5 % | **0.0 %** | 0.0 % | 0.0 % | 0.0 % |

The cliff lands exactly at `D == ubtb_entries`, verified at U = 8, 16 and 32. Wrong-path
fetch acts as a prefetcher for the uBTB (+2.324 % hit rate per extra block, slope
independent of capacity) right up until it evicts the entire structure in one pass — the
classic LRU pathology for cyclic access slightly exceeding capacity.

The same mechanism reproduces on the real CoreMark trace, as a softer version:
uBTB hit 50.58 % (D=0) → **58.12 % (D=10)** → 49.57 % (D=32), turning at
`D ≈ ubtb_entries = 16`, with the mispredict count bit-identical throughout. The
front-end bubble sensitivity band on real workloads is about **±5 %**.

**So "pick a larger D to be safe" is wrong.** `D` must be a swept dimension, and sweeps
must avoid `D ≈ predictor capacity`.

## Honest limitations

- **Absolute cycle accuracy is not validated.** There is no golden reference in this
  repository. What *is* verified: bit-exactness between the bit-sliced build and
  Verilator, and that the retire stream matches Spike. Correlation against gem5 or RTL
  is future work.
- **No memory-level parallelism validation.** CoreMark's working set fits in L1
  (measured: 100.00 % L1D hit rate), so the MSHR model is never exercised. This needs a
  memory-bound workload.
- **Cache latency is pre-resolved offline.** MSHR occupancy and queueing are modelled,
  but bank conflicts, prefetcher training by actual timing, and wrong-path cache
  pollution are not.
- **MSHRs do not merge**, which under-estimates MLP — the opposite bias to the point
  above, but do not expect them to cancel.
- **Rename recovery correctness depends on the ROB flushing only at commit time.**
  Documented as a cross-module invariant; changing the ROB to mid-buffer flush requires
  rename checkpointing.
- **Store-to-load forwarding is not modelled**; disambiguation uses a deterministic
  probabilistic model (~3 %), which systematically over-estimates the penalty.
- Integer x0–x31 only. Wrong-path shadow expansion (`shadow_off`) is not yet generated.

## Reproducing

```bash
# 1. Build Spike and generate a trace (needs a RISC-V ELF)
make -C preprocess/spike
preprocess/spike/run_coremark.sh

# 2. Offline front-end and memory models
preprocess/frontend/build/frontend_sim traces/coremark.frontend.txt \
    preprocess/frontend/config.json traces/coremark.fe
preprocess/cachesim/cachesim traces/coremark.mem.txt \
    preprocess/cachesim/config.default.json traces/coremark.mem

# 3. Compile the model
docker run --rm -v "$PWD/model/ooo":/w -w /w hdlc/yosys:latest yosys -p '
  read_verilog -I. top.v frontend/*.v rename/*.v backend/*.v lsu/*.v
  hierarchy -check -top ooo_top; proc; opt_expr; opt_clean
  flatten; opt -fast; write_json ooo_top.json'
python3 -m bstm_cc model/ooo/ooo_top.json ooo_top --backend c64 -o build/ooo_bs.c

# 4. Verify and run
make -C test equiv        # Verilator ↔ bit-sliced, must be 0 mismatches
make -C test check-real   # end-to-end on the real trace
ci/check_contract.sh      # design-rule enforcement
```

Traces (2.3 GB) are not in the repository; regenerate them with step 1.

## Repository layout

```
doc/PLAN.md        Full design document (Chinese, ~12k characters): prior art,
                   architecture, risk register, phased roadmap, decision records
CONTRACT.md        Interface contract and its change log — every defect found
                   during integration, with the ruling and rationale
model/ooo/         The timing model (Verilog-2005)
preprocess/        Offline: Spike writer, front-end predictor sim, cache sim
tools/bstm-cc/     Yosys JSON → bit-sliced C compiler
lib/runtime/       Reader, transpose, refill, thread pool, statistics
test/              Equivalence harness, unit tests, benchmarks, mock model
ci/                Contract checker, gate-level cost measurement
prototype/         The original 167-line proof of concept
```

`CONTRACT.md` may be the most useful file for anyone attempting something similar. It
records 29 integration defects. **Not one of them was a wrong algorithm inside a module** —
every single one was an interface, semantics or measurement-methodology problem.

## Prior art

BSTM sits in the decoupled functional/timing simulation lineage (Mauer, Hill & Wood's
2002 taxonomy). The closest relatives are **HAsim** (Pellauer et al. — timing partition
in Bluespec, functional partition supplies ISA semantics), **FAST** (Chiou et al. — QEMU
as speculative functional model, Bluespec timing model on FPGA), **RAMP Gold** (host
multithreading: one physical pipeline time-multiplexed across 64 target cores) and
**A-Port Networks**, whose abstraction the model layer adopts directly. The compiler
front-end follows **GEM** (NVIDIA/PKU, DAC 2025) in using Yosys, but stops at word-level
RTLIL rather than descending to AIG, and optimises throughput where GEM optimises latency.

Full citations in `doc/PLAN.md` §15.2.

## Licence

Apache-2.0.
