# preprocess/spike — CoreMark instruction trace → BSTF  (Agent H)

Turns a Spike run of the Andes bare-metal CoreMark image into the BSTF trace
triple consumed by Agent A (offline frontend), Agent B (offline cache) and the
Verilog timing model.

```
spike -l --log-commits ──stdout──▶ bstf_gen ──▶ coremark.bstf
                                            ├─▶ coremark.frontend.txt
                                            ├─▶ coremark.mem.txt
                                            ├─▶ coremark.imem.txt
                                            ├─▶ *.meta.json  (one per file)
                                            └─▶ coremark.audit.txt / .mismatch.txt
```

## Quick start

```sh
make                      # build bstf_gen + bstf_check
make test                 # 200k-instruction self test (must print PASS)
./run_coremark.sh --small # traces/coremark_1m.*   1M instructions
./run_coremark.sh --full  # traces/coremark.*     50M instructions (~2 min)
```

Nothing outside `preprocess/spike/` and `traces/` is ever written.
`/home/toyman/work/andes-spike/` and `/home/toyman/work/mc_run_cmv5/` are
treated as strictly read-only.

---

## 1. Route chosen, and why

**Route: real Spike (the Andes fork already built at
`/home/toyman/work/andes-spike/build/spike`), driven through its own
`-l --log-commits` text commit log, parsed by a C++ streaming converter.**

The alternatives and why they lost:

| option | verdict |
|---|---|
| patch Spike to emit binary records | would mean touching someone else's repo (forbidden) or maintaining an out-of-tree build of a 300 MB binary; the text log already carries everything needed |
| link against `libriscv.so` with our own driver | fastest in principle, but `sim_t`'s constructor, `cfg_t`, HTIF wiring and the Andes `--dut` plumbing are private, fast-moving API surface — high risk for zero functional gain here |
| `qemu-riscv64 -d in_asm` | `qemu-riscv64` on this box is the **Linux-user** emulator; the CoreMark image is bare-metal (entry 0x0, custom `tohost` at 0x100000, no syscalls). Wrong tool. |
| write our own RV64 functional simulator | would have to re-implement M/A/F/D/Zb* and the Andes extensions and then be trusted; Spike is the golden model the rest of Andes already cosims against |

The text log costs ~210 bytes/instruction but never touches disk (it is piped),
and the whole pipeline sustains **~500 k instructions/s** — 50 M instructions in
about two minutes. That was not worth optimising away.

**The decoder is deliberately independent of the log.** `rv64_decode.h` decodes
the raw instruction word for class / registers / memory size / branch type /
branch displacement; the commit log supplies the dynamic facts (PC stream,
effective addresses). Every record is then cross-checked one against the other,
which is how the two real bugs below were caught.

### Getting Spike to run this image

The image is not a `pk` program; it is a bare-metal ELF linked at address 0.
Three things were needed:

1. `--csr-marchid 0x8000000000008a66` — the Andes fork aborts with
   `Unknown marchid: 0x0` otherwise. Value copied from `mc_run_cmv5/iss.cfg`.
2. `--reset-vector=0x60000000` — Spike puts its boot ROM at `0x80`, which is
   inside `.text`. Moving the reset vector out of the way is enough because we
   also pass `--pc=0x0`.
3. `--isa=rv64imafdc_zicntr_zihpm_zba_zbb_zbc_zbs_xandes` — **`zicntr` is not
   optional.** `time()` at `0xa8` is `rdcycle a5`. Without `zicntr` that traps,
   `mtvec` is 0, so the core restarts at `_start` and CoreMark loops forever.
   (This produced a 119 M-instruction "trace" that was really the same 1 M
   instructions over and over. Symptom to remember: `_pass` at `0x90` never
   retires.)

Memory map passed to Spike: `-m0x0:0x200000,0x70000000:0x100000,0x80000000:0x10000`
(`.text`/`.text.init` and `tohost` @ 0x100000; `.data`/`.stack`; `.data.io`).

**Confidence check:** with this configuration the whole program retires
**961,438** instructions. The reference Andes RTL cosim log
(`mc_run_cmv5/spike_trace.log`, produced months ago by a completely different
flow) ends at `#961,432`. The two agree to within the six instructions of the
`_finish`/`_pass` epilogue.

### Reaching 50 M instructions

The shipped ELF is built for **4 CoreMark iterations** = 951,300 steady-state
instructions, which is far short of 50 M. CoreMark reads its iteration count
from `seed4_volatile` (`core_main.c`: `results[0].iterations = get_seed_32(4)`),
and on this bare-metal port that variable is a plain `.data` initialiser.

`patch_iterations.py` rewrites that one 32-bit word **in a private copy** of the
ELF (`preprocess/spike/elf/coremark_v5_i256.exe`); the original is never
modified. Verified linear: 4 iterations → 951,300 instructions,
8 → 1,901,723, i.e. ~237,600 instructions per iteration. `--full` uses 256
iterations (~60.8 M available) and takes the first 50 M.

Because both traces come from the same 256-iteration ELF and the same start
point, **`coremark_1m.*` is an exact prefix of `coremark.*`**.

### Where the trace starts (skipping init)

`--start-pc 0xf4` = `start_time`, the first instruction of CoreMark's timed
region, i.e. after `portable_init`, `core_init_matrix`, `core_init_state` and
`core_list_init`. `icount_start = 10137` records how many instructions were
dropped. `--stop-pc 0x90` = `_pass`, the terminal `beqz zero, _pass` spin.

---

## 2. Output formats

### `<name>.bstf`

`bstf_hdr_t` (232 bytes) followed by `n_records` records of `rec_bytes` bytes.
See §4 for the record-size caveat.

* `uop_class` / `exec_lat` — per the spec: ALU 1, MUL 3, DIV 12, FPU 4,
  LOAD/STORE 1, BRANCH/JUMP/JALR/RET/CSR/FENCE/AMO/NOP/SYS 1.
  Calls are **not** a separate class: `jal ra,…` is `UC_JUMP` + `BF_CALL`,
  `jalr ra,…` is `UC_JALR` + `BF_CALL`, `ret` is `UC_RET` + `BF_RET`.
* `src1/src2/dst` — v2/A4 encoding: `bit7` = valid, `bit[5:0]` = architectural
  register number, `bit6` always 0. Integer registers only.
* `flags` — `BF_SERIALIZE` (CSR/SYS/FENCE), `BF_FENCE`, `BF_BLK_END`,
  `BF_INTERVAL_END` (last record only), `BF_CALL`, `BF_RET`.
  `BF_CALL`/`BF_RET` follow ISA manual §2.5.1 exactly, including the
  `rd,rs1 ∈ {x1,x5}, rd≠rs1` co-routine case that sets both.
* `shadow_off` / `shadow_len` — **0 in this version** (wrong-path expansion is
  the next round's work), as instructed.

**Cursor semantics — CONTRACT G4.** Both overlay indices are **0-based** and
use the same formula:

```
idx(i) = ( Σ_{j≤i} delta(j) ) − delta(0)
```

so a straight reader is:

```c
uint64_t fe_sum = 0, mem_sum = 0;
uint64_t fe_d0 = rec[0].fe_index_delta, mem_d0 = rec[0].mem_index_delta;
for (i = 0; i < n_records; i++) {
    fe_sum  += rec[i].fe_index_delta;
    mem_sum += rec[i].mem_index_delta;
    fe_blk = fe[fe_sum - fe_d0];                       // always valid
    if (rec[i].uop_class == UC_LOAD ||                 // <-- key off the class,
        rec[i].uop_class == UC_STORE ||                //     NOT off the delta
        rec[i].uop_class == UC_AMO)
        mem_ev = mem[mem_sum - mem_d0];
}
```

Consequences, both asserted by `bstf_check`:

* `fe_index_delta` is 1 on the first instruction of every fetch block,
  **including record 0** (it opens block 0), so `delta(0) == 1` and
  `Σ fe_index_delta == n_fe_blocks`.
* `mem_index_delta` is 1 on every memory-accessing instruction **except the
  first one in the trace**, which carries 0 because it *is* index 0. Hence
  `Σ mem_index_delta == n_mem_access − 1`.
* Because the first accessing instruction's delta is 0, **the delta alone no
  longer tells you whether a record consumes a `.mem` entry** — use
  `uop_class ∈ {UC_LOAD, UC_STORE, UC_AMO}`. `bstf_check` asserts that the
  count of such records is exactly `n_mem_access`, so this is safe.
* Highest index actually used is `n_fe_blocks − 1` / `n_mem_access − 1`;
  `bstf_check` replays the whole trace and asserts both, plus zero
  out-of-range accesses.

### `<name>.frontend.txt` — one row per retired instruction (Agent A)

**Format version 2 — 7 columns.**

```
<pc_hex> <is_branch> <br_type> <taken> <target_hex> <is_block_end> <next_pc_hex>
             0/1      0=cond 1=jal        (see below)                (see below)
                      2=jalr 3=ret 4=call
```

Rows are in program order and correspond 1:1 with the `.bstf` records.
Lines beginning with `#` are comments (the file carries a 6-line header).

* **`target_hex` is the architectural branch target and is valid regardless of
  `taken`.** It is decoded from the instruction's immediate, so a *not-taken*
  conditional branch still reports where it *would* have gone — which is what a
  BTB needs in order to be trained/validated. For indirect transfers
  (`jalr`/`ret`, always taken) it is the PC Spike actually went to. Non-branch
  rows carry 0. It is **not** a fetch-redirect signal: do not treat
  `target != 0` as "this branch jumps".
* **`next_pc_hex` is the fetch ground truth**: the PC of the next instruction
  that actually retires, i.e. column 1 of the next row. `0` on the final row
  means end of trace. Present on **every** row, not just branches, so the
  consumer can also validate block boundaries.

The two columns exist because they answer different questions, and neither can
be derived from the other in this format: the fall-through is `pc + ilen`, and
RVC makes `ilen` 2 or 4 bytes, which the format does not carry.

```
136e 1 0 0 13a8 1 1372     not-taken branch: would jump to 13a8, actually fell through to 1372
fc   1 4 1 a8   1 a8       taken call: target == next_pc
4fd0 0 0 0 0    1 0        last row of the trace
```

`bstf_check` asserts every row has 7 columns, that `next_pc` of each row equals
`pc` of the next row, and that the final row's `next_pc` is 0.

### `<name>.mem.txt` — data accesses only (Agent B, B1/B2)

```
<vaddr_hex> <size_bytes> <is_store> <is_ifetch=0> <pc_hex>
```
Program order; row *k* is the access of the *k*-th instruction with
`mem_index_delta == 1`. `pc_hex` is the accessing instruction's PC, for
PC-indexed stride prefetching.

### `<name>.imem.txt` — instruction fetch (B2)

```
<vaddr_hex> <64> <0> <is_ifetch=1> <pc_hex of the fetch block's first insn>
```
One row per fetch block, address aligned to the 64 B line. A block emits a
second row when its bytes straddle a line boundary — possible with RVC, since a
4-byte instruction may start at a 2-byte offset. That is why `imem.txt` has
16,443,152 rows for 15,690,426 fetch blocks (4.8 % straddles).

### `<name>.*.meta.json` (B3)

One sidecar per output file: `format_version` for that file's own layout
(`frontend` is at 2, `dmem`/`imem` at 1), generator + version, generation timestamp, source
ELF path and **sha256**, the exact Spike command line, the instruction range
(`icount_start` … `icount_end`), row/record counts, and a
`config_fingerprint` string (`fetch_width`, `line_bytes`, `split_line`,
`rec_bytes`, `skip`, `start_pc`, `stop_pc`, `count`).

### `<name>.audit.txt` / `<name>.mismatch.txt`

100 uniformly-sampled records (reservoir sampling) printed with both the decoded
fields and the Spike commit-log fields they were checked against, and every
disagreement (capped at 50). **Both mismatch files are 0 bytes.**

---

## 3. Fetch block definition

A fetch block ends when **any** of:

1. the instruction is a taken control transfer, or
2. the block already holds `--fetch-width` (4) instructions, or
3. the next PC is not sequential (trap / redirect), or
4. the next instruction falls in a different 64 B i-cache line
   (`--no-split-line` turns this off).

Rule 4 is an addition to the brief. Without it a block could span two cache
lines and "one i-fetch row per fetch block" would be ill-defined; with it, a
4-wide fetch engine's block never needs two tag lookups. It costs a little
average block length (3.187 instead of ~3.3) and is the reason 17 % of blocks
are a single instruction.

---

## 4. Known limitations / things to watch

* **`shadow_off`/`shadow_len` are 0.** No wrong-path expansion yet.
* **`sizeof(bstf_rec_t)` was 18 bytes** in bstf.h v1 (the trailing `rsvd1`
  overflowed the documented 16). Fixed by the supervisor in v2; `bstf_gen` now
  refuses to run if `--rec-bytes` disagrees with `sizeof(bstf_rec_t)`.
  Consumers should still trust `hdr.rec_bytes`, not `sizeof`.
* **`sizeof(bstf_hdr_t)` is 232**, not a multiple of 16, so records are not
  16-byte aligned in the file. `bstf_rec_t` is packed so this is correct, but
  `mmap` + cast will be unaligned. `workload` sits at offset 72 and `isa` at
  136 — a hand-written parser that assumes natural alignment instead of the
  packed layout will read them as empty.
* **FP operands are dropped** (A4). Harmless here: the image is a soft-float
  build and executes zero FP instructions, so `FP operands dropped = 0`.
* **No DIV in the trace.** The only `divw` in the binary is in `time_in_secs`,
  which runs after the timed region. `UC_DIV` is exercised by the decoder but
  not by this workload.
* **`exec_lat` for FP divide/sqrt would be wrong** if an FP workload were ever
  traced: the spec gives a flat FPU = 4.
* **Vendor extensions**: `Xandes` instructions are decoded by the fallback path
  (class from the commit log). CoreMark contains none, so this path never
  fires here (`unrecognised encodings = 0`).
* The log parser depends on the Andes fork's commit-log layout (`core N: mem[pc]
  == insn (disasm)`, indented `mem[…] ==/<=` and `reg ==/<=` lines). Upstream
  Spike does **not** log register *reads*; only the cross-check uses them, so
  upstream Spike would still work with `--audit 0`-style checking degraded.

---

## 5. Files

| file | role |
|---|---|
| `rv64_decode.h` | standalone RV64GC + Zba/Zbb/Zbc/Zbs decoder (class, regs, mem size, RAS hints, branch displacement) |
| `bstf_gen.cc` | commit-log parser + BSTF/frontend/mem/imem/meta writer |
| `bstf_check.cc` | independent validator: re-derives every header field from the records and cross-checks the text files |
| `patch_iterations.py` | rewrites `seed4_volatile` in a private copy of the ELF |
| `run_coremark.sh` | the whole pipeline (`--self-test` / `--small` / `--full` / `--whole`) |
| `elf/` | generated, iteration-patched ELF copies |
| `selftest/` | generated, `make test` output |
