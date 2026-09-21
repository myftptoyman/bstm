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

**Integrated model**

```
ooo_top:  38,549 cells   0 memories   0 latches   0 variable shifts
```

Gate-level (the honest metric — `proc; opt` cell counts are misleading by up to 12×):

| module | proc/opt cells | gate-level gates |
|---|---:|---:|
| `be_iq` | 7,852 | **93,707** |
| `lsu_q` | 12,743 | 27,949 |
| `rn_rename` | 4,761 | 18,883 |

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

**CoreMark trace** (real Spike, 50 M instructions from steady state):

```
ALU 46.52 %  LOAD 22.77 %  BRANCH 21.21 %  STORE 4.40 %  MUL 3.96 %
memory ops 27.17 %   fetch-block average 3.19 instructions
```

Validated by decoding all 50 M instructions with an independent decoder and comparing
against Spike's commit log: 0 mismatches on encodings, memory presence, memory size,
registers and branch targets. Total retired instruction count agrees with a pre-existing
Andes RTL co-simulation log to within the epilogue (961,438 vs 961,432).

**Front-end predictor**, on synthetic traces:

```
TAGE conditional-direction mispredict   0.258 %   (bimodal: 32.26 %, 125× worse)
RAS accuracy on a 20-deep call chain    79.96 %   ≈ 16/20, exactly the 16-entry limit
```

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
