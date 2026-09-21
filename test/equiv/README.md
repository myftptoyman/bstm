# test/equiv — Verilator ↔ bit-sliced 逐 cycle 對拍

PLAN §9.2 的第一層驗證，整個專案的安全網。

```sh
./run_equiv.sh [每階段 cycle 數]      # 預設 500000
make run N=1000000
```

## 設計

```
        同一份 .v
           ├──► Verilator（1 instance, word-level）
           └──► bit-sliced C（lane 0；其餘 63 lane 灌不同的雜訊 / 不同 cfg）
                     │
        每 cycle 比對所有 output port + 所有 counter
        第一個不同 → 報 phase / cycle 號 / 訊號名 / 兩邊的值 / xor
```

- **Verilator 一律用 `--x-assign 0 --x-initial 0`**（在 `Makefile` 的 `XFLAGS`）。
  沒有這兩個 flag，X 語意差異會製造大量假警報。
- 其他 63 個 lane 故意灌不同的輸入與不同的 `cfg_*`，順便驗證沒有跨 lane 污染。
- 比對時機：`clk=0 eval()` 之後比組合輸出，`clk=1 eval()` 之後比 counter。

## 測試向量

| | chain.v | mock_ooo_top.v |
|---|---|---|
| (a) LFSR 隨機 | ✓ | ✓ |
| (b) 真實 `.bstf` 前 N 筆 | ✓（uop class/lat/dst 映射到 tag/lat） | ✓（走完整 `bstm_refill` 路徑） |
| (c) 定向邊界 | credit 用盡 / 管線排空 / 滿吞吐 / credit 飽和 / rst 夾在流量中 | ROB=1 永遠滿、ROB=64 滿吞吐、fetch_width=1、commit=1、valid=0 斷流、每拍誤預測、REDIRECT 但預測正確、occ==rob 邊界、rst 與滿流量同拍 |

## 結果（2026-09-21，N = 500000）

```
--- 1) prototype/chain.v (830 cell, 32-stage) ---
equiv chain.v                  521200 cycles,   1563600 signal-compares, 0 mismatches
  活動量: in_ready 高 19.4%, out_valid 高 1.9%, out_tag 走過 64/64 個值

--- 2) test/mock/mock_ooo_top.v (ooo_top 埠列 + 11 個 48-bit counter) ---
equiv mock_ooo_top.v           514142 cycles,   7197988 signal-compares, 0 mismatches
  活動量: sum(fb_take)=1021417, fb_take 取過 0/1/2/3/4, redirect 59432 次（跳 shadow 29717）
EQUIV PASS
```

真 trace（Agent H 的 `traces/coremark_1m`，overlay 還沒產生所以用 `BSTF_LAX=1`）：

```
BSTF_LAX=1 ./equiv/build/equiv_mock 200000 ../traces/coremark_1m
equiv mock_ooo_top.v           403574 cycles,   5650036 signal-compares, 0 mismatches
```

## 框架抓到的第一個真 bug

`./build/equiv_chain -r`（打開 rst 階段）會在 **cycle 36 / `in_ready`** 失敗：

```
MISMATCH  phase=lfsr-random  cycle=36  signal=in_ready
          verilator = 0x0 (0)
          bit-sliced= 0x1 (1)
```

根因：**`prototype/gen.py:156` 算了 `rv = "VZERO"` 但從來沒用**。
產生的 FF 更新式是

```c
s->qN = (D & (EN & ~SRST)) | (p->qN & ~(EN & ~SRST) & ~SRST);
```

不管 `$sdff` 的 `SRST_VALUE` 是多少，reset 一律歸 0。
而 `prototype/stage.v` 的 `credit` 是 `reset <= {CREDIT_W{1'b1}}` = 15。
→ **`tools/bstm-cc` 必須讀 `SRST_VALUE` 參數**，否則所有非零 reset 值都會靜默錯誤。

在修好之前，預設模式完全不 assert `rst`：Verilator 的 `--x-initial 0` 讓兩邊都從
全 0 起跑，`chain.v` 的同步 reset 不觸發，對拍成立。
`mock_ooo_top.v` 的所有暫存器 reset 值都是 0，所以它的 rst 階段是打開的。

## 產生物

`gen/chain_bs.c` 是 `prototype/cbench.c`（yosys + `prototype/gen.py` 的輸出）
去掉 benchmark 的 `main()`、把 `eval_cycle` 改成外部符號而來。
本機沒有 yosys；裝好之後跑 `gen/regen.sh` 可重新產生。
Agent C 的 `tools/bstm-cc` 完成後，把 `regen.sh` 裡的 `gen.py` 換掉即可。

## 接真模型的做法

`equiv_mock.cpp` 就是模板：把 `Vmock_ooo_top` 換成 `Vooo_top`、
`mock_ooo_bs.c` 換成 bstm-cc 產生的 `ooo_top_bs.c`、counter 名單換成 top.v 的 11 個，
其他（驅動、比對、報告、三種測試向量）都不用動。
