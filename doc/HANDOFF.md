# BSTM 交接摘要

最後更新：2026-09-23　　HEAD：`cc8acba`（尚未 push，見 §5）

---

## 1. 這個專案在做什麼

用 **Verilog 寫 timing model**（不是 RTL），經 **Yosys 出 word-level netlist**，再編成
**bit-sliced C**：一個 64-bit 機器字 = 64 個「同時在跑的處理器實例」，一次 bitwise 運算
就把同一個 gate 在 64 個 config／64 段程式上同時算完。ISA 層的行為不在模型裡 ——
**Spike 離線先跑完，結果存成 trace**，模型只吃 trace。

所以這是 Mauer/Hill/Wood 分類裡的 **functional-first / trace-driven**，而且是
**離線**的：ISS 和 timing model 不在同一個 process，中間沒有 lockstep。

**為什麼值得做**：單一 instance 的 bit-sliced 模擬比 Verilator **慢** 1.8–5.5×
（bit-serialization tax）。全部的贏面來自 64 lane 的乘數 —— 淨值 12–35×。
這是給 **design-space exploration** 用的，不是給單點模擬用的。

計劃書（13,083 字，27 章）：`doc/PLAN.md`。

---

## 2. 目前的樣子

### 2.1 模型

4-issue out-of-order，10 stage，7 個模組：

| 檔案 | 內容 |
|---|---|
| `model/ooo/top.v` | 接線 + 所有 counter（**契約檔，我擁有**） |
| `model/ooo/common/ifc.vh` | 所有 `define`、DUOP/RUOP 編碼（**契約檔，我擁有**） |
| `model/ooo/frontend/fe_front.v` | fetch、TAGE、uBTB 16 / L2 BTB 8K、RAS |
| `model/ooo/rename/rn_rename.v` | RAT + free list |
| `model/ooo/backend/be_dispatch.v` | dispatch、partial accept |
| `model/ooo/backend/be_iq.v` | issue queue、wakeup matrix |
| `model/ooo/backend/be_eu.v` | 執行單元、completion wheel |
| `model/ooo/backend/be_rob.v` | ROB、commit、flush |
| `model/ooo/lsu/lsu_q.v` | LDQ/STQ/MSHR |

整合後 **29,844 cells、0 memory、0 latch、0 variable shift**（`cc8acba` 重量）。
gate-level：**148,046 gates / 8,158 flops / 156,204 cells**（`ci/gate_count.sh`）。

### 2.2 建模規則（違反就會壞，而且是靜默地壞）

1. 欄位 ≤ 8 bit —— bit-serialization 成本正比於欄位寬度
2. **最大尺寸 + runtime mask**，不是「每個 config 改一份檔案」（CONTRACT §20）
3. 純同步，不准 latch、不准非同步記憶體
4. `case` 一定要有 `default`
5. counter 只存在於模型裡，不進 netlist 的關鍵路徑
6. **多位元欄位一律 `` `FIELD +: 寬度 ``，不准寫 `[hi:lo]`**（CONTRACT §20，這是 v4 才加的）

### 2.3 工具鏈

| 路徑 | 做什麼 |
|---|---|
| `tools/bstm-cc/` | Yosys JSON → bit-sliced C。liveness + graph coloring 做 slot 配置 |
| `lib/runtime/refill.c,h` | trace 游標 + **wrong-path shadow 狀態機**（核心，見 §3） |
| `lib/runtime/bstf_reader.h` | trace 讀取 |
| `include/bstf.h` | **trace 格式契約**（16-byte record、232-byte header，packed） |
| `preprocess/spike/` | Spike → 原始 trace |
| `preprocess/frontend/` | 分支預測器離線模擬 → `.fe` / `.fe.wp` |
| `preprocess/cachesim/` | cache 離線模擬 → `.mem` |
| `test/tools/bstfdump.c` | trace 檢查器 |
| `doc/HANDOFF.md` | 這份交接摘要 |
| `test/tools/ipc_wp.cpp` | **真模型 + runtime 的 IPC driver**（§4 全部數字出自這支） |
| `test/tools/build_ipc.sh` | 用不同 `define` 建 driver 的 harness |
| `ci/check_contract.sh` | 機械式契約檢查 |
| `ci/gate_count.sh` | gate/flop 計數（`synth -flatten` + `abc`） |

`bstm-cc` 在真模型上的輸出：

```
nets=104061  cells=29844  state=8944  ops=112857  slots=3306
```

liveness 把 104,061 net 壓到 3,306 slot（31.5×），working set **98 KB**
（26 KB slot + 72 KB state；目標是 512 KB L2）。

產生網表的 yosys 腳本必須是：

```
proc; flatten; opt; memory -nomap; opt -full; write_json
```

**一定要 `flatten`，一定不要 `techmap` / `abc`** —— bstm-cc 吃的是 word-level 網表。

RUOP 重佈局（§4.4）前後用同一份腳本量的對照：

| | `b7f9960`（舊佈局） | `cc8acba`（`+:` 佈局） |
|---|---:|---:|
| cells | 29,864 | 29,844 |
| nets | 111,789 | 104,061 |
| state bit | 9,401 | 8,944 |
| ops | 122,762 | 112,857 |
| slots | 3,334 | 3,306 |

`RUOP_W` 從 40 變 38，所以成本略降。**README 舊版寫的 38,549 cells / 3,432 slots
不是這條腳本量的**，已一併更正。

後端選擇（實測）：

| 後端 | 吞吐 | 編譯時間 |
|---|---|---|
| 一個大 C function + gcc -O1 | OOM（27 GB / 10 min） | — |
| bytecode 直譯器 | 0.68 G ops/s | 即時 |
| 手寫 x86-64 JIT | 1.66 G ops/s | 0.003 s |
| **36 個 split function + gcc -O2** | **5.19 G ops/s** | 287 s |

### 2.4 Trace

`traces/` 有兩份，都是真的 CoreMark：

| | `coremark_1m` | `coremark` |
|---|---|---|
| 指令數 | 1,000,000 | 50,000,000 |
| `.bstf` | 25 MB | 1.2 GB |
| shadow | 9,971,840 B（623,240 rec） | — |
| 帶 shadow 的分支 | 15,581 | — |
| fetch block（`.fe`） | 315,915 | — |
| 資料存取（`.mem`） | 280,052 | — |

overlay 的粒度不一樣，這是 CONTRACT §19：`.fe`/`.fe.wp` **每個 fetch block 一筆**、
`.mem` **每次資料存取一筆**、base record **每條指令一筆**。

---

## 3. Wrong-path（這一輪的主要進展）

以前 `shadow_off` 全是 0，**從來沒有付過誤預測代價**，所有 IPC 都是上界。

現在 `preprocess` 會把誤預測路徑**預先展開**寫進 `.bstf` 的 shadow 區，
`lib/runtime/refill.c` 裡有 per-lane 的狀態機：

```
CORRECT  --(fb_redirect & fb_redir_shadow)-->  SHADOW
SHADOW   --(shadow 供完、flush 還沒到)-->      STARVED
SHADOW/STARVED --(fb_redirect & ~fb_redir_shadow)--> CORRECT
```

**STARVED 不可以自己溜回正確路徑** —— 那會讓模型以為還在推測，卻把正確路徑的
uop retire 掉。

驗證（1M 與 50M 都過）：`cp_consumed` 恰等於 `n_records`、`enter == leave`、
`nobranch = resolved = 0`、`bstm_refill_check()` PASS。
bit-sliced 對拍 Verilator：6,053,610 次訊號比對、0 mismatch。

---

## 4. 結果

全部出自 `test/tools/ipc_wp.cpp`，真 CoreMark，Verilator（純量，只開 lane 0）。
`BSTM_NOWP=1` 是「收到 redirect 但不跳 shadow」的對照組 = 舊的量法。

### 4.1 Wrong-path 的代價

完整 1M trace，最佳 config（PRF 96 / ROB 128 / LSQ 32 / IQ 64）：

| | IPC | cycles | mispred redirect |
|---|---:|---:|---:|
| 無 wrong-path | 1.497 | 668,051 | 0 |
| **有 wrong-path** | **1.263** | 791,915 | 10,732 |

**掉 15.6%**，而且這是**下界**（原因見 §4.5）。

### 4.2 PRF sweep（ROB 64 / LSQ 16 / IQ 32，500K cycles）

| PRF | IPC (WP) | IPC (無 WP) | freelist stall | ROB 佔用 |
|---|---:|---:|---:|---:|
| 64 | 1.240 | 1.435 | 20,757 | 25.1 |
| 96 | 1.269 | 1.447 | **0** | 26.5 |
| 128 | 1.269 | 1.447 | 0 | 26.5 |
| 192 | 1.269 | 1.447 | 0 | 26.5 |
| 256 | 1.269 | 1.447 | 0 | 26.5 |

freelist 壓力**在 96 精確歸零**，與第一性原理界限一致（32 已 commit 映射 +
≤ `ROB_N`=64 in-flight）。96 以上逐位元相同。

> **這張表在 `cc8acba` 之前是無效的**，見 §4.4。

### 4.3 結構 sweep（wrong-path 開，500K cycles）

| config | IPC (WP) | IPC (無 WP) | ROB 佔用 | IQ stall | LSQ stall | backpressure |
|---|---:|---:|---:|---:|---:|---:|
| PRF64 / ROB64 / LSQ16 / IQ32 | 1.240 | 1.435 | 25.1 | 63,265 | 67,915 | 130,485 |
| PRF96 | 1.269 | 1.447 | 26.5 | 63,327 | 71,152 | 138,894 |
| PRF96 + ROB128 | 1.276 | 1.451 | 26.4 | 63,419 | 71,474 | 134,180 |
| PRF96 + LSQ32 | 1.299 | 1.473 | 31.2 | 122,103 | **2,521** | 133,043 |
| PRF96 + IQ64 | 1.271 | 1.448 | 30.3 | **0** | 89,893 | 119,475 |
| **全開** | **1.310** | 1.483 | 38.8 | 45,190 | 68,926 | 114,055 |

每一列都是**瓶頸轉移，不是消除**。IQ64 把 IQ stall 打到 0，整包轉給 LSQ；
LSQ32 反過來。四個全開只買到 5.6%。

### 4.4 RUOP 欄位的靜默截斷（本輪最重要的發現）

`RUOP_S1/S2/D` 從第一個 commit 起就是**寫死的 6 bit**（`21:16` / `14:9` / `7:2`），
而 `rn_rename.v` 寫進去的是 `` `PRF_W `` 寬的值。`PRF_N > 64` 時 Verilog 靜默截斷高位
→ 實體暫存器編號別名 → rename→IQ→EU→ROB 相依鏈全錯 → LSQ 永久 full、IPC 0.004。

三層驗證全部放過：

| 驗證 | 為什麼沒抓到 |
|---|---|
| `verilator --lint-only -Wall` | 隱式位元寬度截斷不是 warning |
| free list 自測 | 只查可配置數 = `PRF_N - 32`，不經過 RUOP 編碼 |
| 模組級 TB | agent 在 `/tmp/agente_prf/prf7,prf8/` **手改了 per-config 的 `ifc.vh`**（`RUOP_W 48`、`RUOP_S1 24:18`），TB 當然會過 —— 但那份重佈局從來沒進 repo，而 `ifc.vh` 是契約檔 |

**後果**：`dd31c15` 發布的 PRF 表來自那個 scratch，不是整合模型跑 CoreMark。
整合模型在 repo 裡**從來沒有能力表示編號 ≥ 64 的實體暫存器**。

`cc8acba` 已修：欄位基底由 `` `PRF_W `` 算，一律 `+: 寬度` 存取。
PRF=64 下逐位重現 1.240 / 1.435，確認重佈局行為保持。
裁決寫進 CONTRACT §20。

### 4.5 目前卡住上限的結構限制

`top.v` 的 `fb_fe_event` 只有 **8 bit**，載的是 window **第 0 筆**那個 fetch block 的事件。
4-wide window 會整塊跨過後面的 block：

```
blk: seen=302,136  skipped=13,779 (4.4%)   →  mispred 10,732 / 15,581 = 68.9% 覆蓋率
```

所以 §4.1 的 15.6% 是**下界**。要修得把 `fb_fe_event` 改成 per-slot（`` `W*8 ``，
跟 `fb_mem_event` 一樣），那是 `top.v` 的 port 變更 —— 契約檔。

---

## 5. 未完成

依重要性排序：

1. **`fb_fe_event` 改 per-slot**（`top.v`）。離「真的 model 了一個 4-issue OOO」最近的一步。
   改完 §4 所有數字都要重跑。
2. **`.mem.wp` 做成二進位**。目前只有 `traces/coremark_1m.mem.wp.txt`，
   wrong-path 的記憶體事件是**餵 0** —— cache 污染完全沒模。
3. **wakeup matrix 的裁決**。`be_iq` 的 `e_pend` 是 `IQ_N × PRF_N` 的遮罩
   （IQ64/PRF256 時 = 16,384 bit，佔 `be_iq` 的 79%）。
   CONTRACT §1 寫「不准 tag CAM」，需要先裁定這算不算，**IQ 和 PRF 才能一起掃**。
4. **push**。`cc8acba` 加上這次的 README/HANDOFF 都還在本機。
   remote：`git@github.com:myftptoyman/bstm.git`（public）。
5. ~~重量 gate count~~ —— **已做**（§2.3）。README 與 `doc/PLAN.md` 已更正。
6. **bit-sliced 路徑重跑 sweep**。§4 全部是 Verilator 純量跑的。
   64-lane 的優勢要用 `tools/bstm-cc` 那條路才量得到。
7. **模型還缺的東西**（這是架構層級的，不是 bug）：
   - EU 的 port 結構與 bypass network
   - store-to-load forwarding（目前所有 store 都當 store-buffer hit）
   - memory-bound 的 workload（CoreMark 的 L1D 命中率是 100%，MSHR 從來沒滿過）
   - 指令足跡：141,317 ops ≈ 800 KB，對上 32 KB L1i —— 這是 host 端的瓶頸

現況：35.9% 的 issue slot 利用率、L1D 100% 命中。真實 4-issue OOO 是 2.0–2.5 IPC。
**天花板在模型，不在被模擬的設計。**

---

## 6. 重現方式

```bash
cd ~/work/bstm

# 建一個 driver（不給參數就是 ifc.vh 的預設值）
OUT=/tmp/bstm_ipc ./test/tools/build_ipc.sh best \
    PRF_N=96 PRF_W=7 ROB_N=128 ROB_W=7 IQ_N=64 IQ_W=6 LDQ_N=32 STQ_N=32 LSQ_W=5

# 跑（cycles 給大一點就會跑到 trace 結束）
#   參數： <trace_base> <cycles> <rob> <lsq> <iq>
/tmp/bstm_ipc/r_best traces/coremark_1m 100000000 128 32 64

# 對照組：收到 redirect 但不跳 shadow
BSTM_NOWP=1 /tmp/bstm_ipc/r_best traces/coremark_1m 100000000 128 32 64
```

ROB / LSQ / IQ 可以**不重建**、直接用命令列參數往下調（`cfg_*` 埠，CONTRACT §10 要求
全範圍都要能跑）。**PRF 沒有 cfg 埠，只能重建。**

檢查 trace：`test/tools/bstfdump`。契約檢查：`ci/check_contract.sh`。

---

## 7. 最該先讀的東西

**`CONTRACT.md`**（§0–§20）。它記了整合過程中約 40 個缺陷、每一個的裁決和理由。
比模型本身有價值 —— 模型可以重寫，那些坑會重踩。

特別是：

| § | 內容 |
|---|---|
| §8 | stall 歸因原則（`cnt_st_*` 互斥、`cnt_lost_*` 算 uop-slot） |
| §10 | `cfg_*` 必須全範圍可用 |
| §11 | **驗證工具會說謊**（5 個實例） |
| §14 | 結構上不可能 > 容量足夠 |
| §16 | wrong-path 的近似 |
| §18 | 分離的 overlay 需要真執行順序的索引 |
| §19 | overlay 粒度 |
| §20 | **per-config 手改檔不算參數化；模組 TB 過了不代表跨模組編碼契約對** |

另外 `doc/PLAN.md` 第 16 章列了七處「計劃被自己的實作打臉」的地方。
