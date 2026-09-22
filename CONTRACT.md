# BSTM Demo 契約 — 所有 agent 必讀，不可擅改本檔

參考：`doc/PLAN.md`。本檔是 PLAN 的可執行子集。

## 0. Demo 目標規格

4-issue out-of-order RV64，TAGE 方向預測器，16-entry uBTB，8K L2 BTB，
三層 cache。跑 CoreMark，驗證 bit-sliced 與 Verilator 逐 cycle bit-exact。

Pipeline（10 級，誤預測懲罰約 12 拍）:
```
F1  F2  DE  RN  DS | IS  EX  WB  CM
fetch   decode rename dispatch | issue exec writeback commit
```

最大尺寸（runtime mask 可縮小，見 §3）:
| 結構 | MAX | 索引寬度 | 備註 |
|---|---|---|---|
| ROB | **128** | **7 bit** | v7：64 → 128，可 sweep |
| IQ | 32 | 5 bit | |
| LDQ / STQ | 16 / 16 | 4 bit | |
| MSHR | 8 | 3 bit | |
| 實體暫存器 PRF | **256** | **8 bit** | v7：64 → 256，可 sweep |
| fetch / issue / commit width | 4 | 3 bit | |

**所有尺寸相關的宣告一律從 `` `XXX_N `` / `` `XXX_W `` 推導，禁止寫死數字。**
這包括：陣列寬度、佔用計數器（需 `` `XXX_W+1 `` 位元才裝得下 MAX 本身）、
零擴展的補零位數、`+ N'd1` 這類常數的寬度、以及 `cfg_*` 埠本身的寬度。

## 1. 五條建模硬規則（Verilog agent 必須遵守，違反即退回）

1. **欄位 ≤ 8 bit，禁止寬算術。** 沒有 32/64-bit 加減。PC/位址/資料不進模型。
2. **最大尺寸 + runtime mask。** 所有結構建 MAX 大小，用 `cfg_*` 控制有效範圍。
3. **純同步單一 clock domain，`posedge clk` + 同步 `rst`。無 latch、無非同步讀取記憶體。**
4. **所有 `case` 必須有 `default`；優先用 if/else 鏈而非 `case`（避免 `$pmux`）。**
5. **所有統計用模型內的 counter 輸出，禁止 `$display`/`$finish`/DPI。**

額外：
- 禁止 `initial`（除 counter 歸零，用 rst）
- 禁止多維 packed array 的非同步讀（會變 `$__RAMGEM_ASYNC_`）
- 陣列一律 `reg [W-1:0] arr [0:N-1]`，讀取走一拍暫存或展開成 mux 鏈

## 2. 檔案所有權（不可跨界寫入）

| Agent | 目錄 | 語言 |
|---|---|---|
| A 前端離線 | `preprocess/frontend/` | C++17 |
| B cache 離線 | `preprocess/cachesim/` | C++17 |
| C 編譯器 | `tools/bstm-cc/` | Python3 |
| D 模型前段 | `model/ooo/frontend/`, `model/ooo/rename/` | Verilog-2005 |
| E 模型後段 | `model/ooo/backend/` | Verilog-2005 |
| F 模型 LSU | `model/ooo/lsu/` | Verilog-2005 |
| G runtime | `lib/runtime/`, `test/` | C11 |
| 監督者 | `include/`, `model/ooo/top.v`, `model/ooo/common/`, `CONTRACT.md`, `doc/` | — |

**共用唯讀**：`include/bstf.h`、`model/ooo/common/ifc.vh`、本檔。要改請回報監督者。

## 3. 每 lane 的配置暫存器（`cfg_*`，模型輸入，測試時固定）

```
cfg_rob_entries   [`ROB_W:0]    `W..`ROB_N     (v7.1: 下限改 `W，見下)
cfg_iq_entries    [`IQ_W:0]     `W..`IQ_N
cfg_ldq_entries   [`LSQ_W:0]    `W..`LDQ_N
cfg_stq_entries   [`LSQ_W:0]    `W..`STQ_N
cfg_mshr_entries  [`MSHR_W:0]   1..`MSHR_N     (MSHR 不受 all-or-nothing 影響)
cfg_fetch_width   [2:0]   1..4
cfg_issue_width   [2:0]   1..4
cfg_commit_width  [2:0]   1..4
```

## 4. 交付標準（監督者驗收）

- Verilog：`yosys -p "read_verilog X.v; hierarchy -top X; proc; opt; check -assert; stat"` 必須通過，
  且 `stat` 中**不得出現** `$__RAMGEM_ASYNC_`、`$mem` 非同步讀、latch
- Verilog：Verilator `--lint-only -Wall` 無 error
- C++/Python：附最小自測，`make test` 或 `python3 -m pytest` 可跑
- 每個交付附一份 `README.md` 說明介面與已知限制

---

# 契約變更記錄

## v2（2026-09-21 16:05）— 由 Agent A / B / D 回報的缺陷觸發

| # | 問題 | 裁決 |
|---|---|---|
| D1 | `RUOP` 排滿，舊 phys reg 無路徑送到 ROB | `cmt_oldprf`→**`cmt_prf`**，語意反轉為「新 mapping」；rename 自建 committed-RAT 推導舊的。附帶：cRAT 成為 flush 復原的正確來源 |
| D2 | `fb_mem_event` 是 fetch 粒度，LSU 在 dispatch 才看到 → 拿到別人的 cache 事件 | `lsu_q` 加 `fb_valid`/`fb_duop`/`fb_take`，自建 mem-event side FIFO |
| A1 | wrong-path byte inline 夾在 `.fe` → runtime 游標每次誤預測錯位 D byte | `.fe` 純 correct-path 線性；wrong-path 另存 `.fe.wp`，第 k 次誤預測 = byte `[k*D,(k+1)*D)` |
| A2 | `bstf.h` 無 call 編碼 → RAS 無法運作 | 新增 `BF_CALL`/`BF_RET`，判定依 RISC-V ISA 2.5.1 的 rd/rs1 規則 |
| A3 | `FE_BUBBLES==3` 未定義 | 保留給「≥3 拍／I-cache miss」，飽和語意 |
| A4 | `src1` 編碼兩處不一致 | 以 `bstf.h` 為準：bit[5:0]=編號、bit7=valid |
| A5 | overlay 無 magic/version | 不加。runtime 檢查「檔案大小 == hdr.n_fe_blocks」，不符即報錯 |
| B1 | stride prefetcher 需要 PC，串流格式沒有 | `.mem` 加第 5 欄 `pc_hex`（實測 L2 hit 0.9992 vs 0.9463） |
| B2 | `.mem` 該不該含 ifetch | **只含 data**。ifetch 另存 `.imem`，demo 不接線 |
| B3 | overlay 無法自我描述配置 | sidecar `<name>.meta.json`，不動二進位格式 |
| — | 監督者自身 bug | `lsu_req_ev` 宣告但無驅動源 → 接地 |
| — | `.fe` 語意變更 | `BSTF_VERSION` 1 → **2** |

## v3（2026-09-21 16:30）— 由 Agent E 回報的阻斷級缺陷觸發

| # | 問題 | 裁決 |
|---|---|---|
| E1 | **ROB 無 backpressure，tail 會蓋掉未 commit 的 entry**（實測 lat=15 → 500 拍 retired=0 卡死） | `be_rob` 加 `output rob_full`，`be_dispatch` 加 `input rob_full` |
| E2 | **`cmt_arf` 無來源** — `RUOP` 不帶架構暫存器編號，ROB 只能用 `cmt_prf[4:0]` 佔位 → **committed-RAT 指到錯 index，靜默毀掉 rename 復原** | `RUOP_W` 32→**40**，新增 `RUOP_ARFD[36:32]`/`RUOP_ARFDV[37]`。bit[31:0] 佈局不動，Agent F 不受影響 |
| E3 | `mshr_full` 無消費者 | 接到 `be_dispatch` |
| E4 | `be_eu` 產生 148 個 `$shift`/`$shiftx` | 改寫成常數索引展開（bit-sliced 下變數位移要 barrel shifter，每 lane O(W log W) mux，代價遠高於 +2.5k cells） |

## 已記錄的跨模組相依（改動時必須回歸）

- **cRAT 復原的精確性綁在 `be_rob` 只做 commit-time flush。** 目前 `be_rob` 是「commit 走到 wrong-path uop 時拉 flush 並清空 ROB 其餘項目」，flush 當下沒有比 flush 點更老的 in-flight uop，故 cRAT 是完整正確狀態 → **復原精確，非近似**。
  若未來改成「ROB 中途 flush」，cRAT 會少掉較老 uop 的 mapping、committed freelist 會把仍在使用的 phys 誤判可配 → rename 會發出仍在使用的實體暫存器。屆時 rename 必須改成 checkpoint 或 ROB walk。

## v4（2026-09-21 17:00）— 由 Agent G 回報，整合期缺陷

### §5 Fetch buffer 與 redirect 語意（補上 top.v 第 20 行引用的缺失章節）

`fb_*` 介面的完整定義：

| 訊號 | 方向 | 語意 |
|---|---|---|
| `fb_valid[W-1:0]` | in | 本拍 fetch window 中哪些 slot 有效 |
| `fb_duop[W*DUOP_W-1:0]` | in | 已解碼的 uop |
| `fb_fe_event[7:0]` | in | **window 第 0 筆所屬 fetch block** 的事件（`FE_*`）。window 若跨兩個 block，第二個 block 的事件在下一拍才看得到 —— 已知限制，見 G7 |
| `fb_mem_event[W*8-1:0]` | in | per-uop 的記憶體事件（`MEM_*`），只對記憶體類 uop 有效 |
| `fb_take[2:0]` | out | 本拍實際消耗幾筆（0..W）。runtime 據此推進游標 |
| `fb_redirect` | out | 本拍要求重導 |
| `fb_redir_shadow` | out | 1 = 跳進 `.fe.wp` / shadow；0 = 回正確路徑 |

**【G2 裁決】`fb_redirect` 指涉哪一筆記錄 —— 必須明確，否則 runtime 與模型會各做各的：**

> `fb_redirect & fb_redir_shadow` 時，runtime 應跳到「**當前游標位置或之後、第一筆帶 `shadow_off != 0` 的記錄**」的 shadow 區。

**不可**用「最近一筆已消耗且有 shadow 的記錄」—— Agent G 實測該解讀會讓 correct-path 游標**倒退**、永遠跑不完（500 萬 cycle 原地打轉、78% retire 是 wrong-path）。上述定義保證游標單調前進。

**【G5】`.fe.wp` 的佈局**：第 k 次誤預測對應 byte 區間 `[k*D, (k+1)*D)`。k 由 `(shadow_off − hdr.shadow_offset)/rec_bytes` 推導，**runtime 不需要知道 D**。

**【G4】overlay 索引基底**：`.fe` 與 `.mem` 的索引都是 **0-based**，`idx(i) = Σ_{j≤i} delta(j) − delta(0)`。
Agent H 目前的 `.mem` 第一筆 delta 已是 1，導致索引跑 1..N 而非 0..N−1（`.fe` 正確），**會讀出界**。以本條為準，由 H 修正。

**【G3】** `top.v` 引用的 CONTRACT §5 即本節。

### §6 valid/ready 協定（由 Agent E 回報）

`ready=0` 時 **`valid` 不得撤回**。違反會讓下游的 stall counter 漏記。
（已確認 Agent D 的 `rn_valid = de_valid & {W{enough_free}}` 不依賴 `rn_ready`，符合。）

### §7 其他裁決

- **【G1】`bstm-cc` 必須讀 `SRST_VALUE`/`ARST_VALUE`。** 原型 `gen.py` 忽略它，非零 reset 值會靜默算錯（`stage.v` 的 `credit` reset=15 在 cycle 36 炸）。
- **【G8】`DUOP_D` 5 bit vs `DUOP_S1/S2` 6 bit 不一致** — demo 只用 x0..x31 整數，不咬人。FP workload 需修。已知限制。
- **【F】`cfg_ldq_entries`/`cfg_stq_entries` 必須 ≥ 4**（dispatch 是 all-or-nothing 收 W=4）。
- **【F】`done_dv` 不存在**：store 沒有 dst，`be_eu` 用自己的 `mem_dv[rob]` 表判斷。此約定為契約。
- **【F/監督者】成本度量一律用 gate-level**（`ci/gate_count.sh`），不得用 `proc; opt` 的 cell 數。實證：`be_iq` cells=7,852 但 gates=93,707（11.9×），`lsu_q` cells=12,743 但 gates=27,949（2.2×）—— cell 數會給出完全相反的排序。

## v5（2026-09-21 19:15）— demo 完成後的收尾

| # | 問題 | 處置 |
|---|---|---|
| 30 | `tools/bstm-cc/Makefile` 的 `ooo` target 缺 `mkdir -p build`，`make clean` 之後整個驗證流程在乾淨 checkout 上跑不起來 | 需補一行。監督者曾誤判成「Agent C 正在重新產生檔案」 |
| 31 | **監督者的單位錯誤**：把工作集算成 `3,432 slot × 64 lane × 8 B = 6.5 MB`，據此宣布「塞不進 L2、方案可能不划算」。正確值 `3,432 × 8 B = 27 KB` —— 一個 `vec_t` 本身就是 64 lane，不可再乘 | 差 64 倍，結論完全相反。與 F 發現的 cell-vs-gate 是同一類錯誤，30+ 個缺陷裡第二次。**任何效能數字必須附推導過程** |
| — | 監督者違反自己訂的所有權規則，在 `tools/bstm-cc/build/` 裡做實驗，被 `make clean` 清空 | 產物要先複製到自己的空間 |

### 最終驗收

```
ooo_top（38,549 cells）bit-sliced vs Verilator
  test_bit_exact_vs_verilator ... ok
  test_working_set_fits_l2    ... ok
  Ran 2 tests — OK
```


## v6（2026-09-21 20:00）— stall 歸因

| # | 問題 | 裁決 |
|---|---|---|
| 33 | `cnt_st_rename` 把下游回壓（IQ/LSQ/ROB/MSHR 滿）記在自己頭上，與下游 counter 重複計數 | 拆成 `cnt_st_rename`（只計 freelist）+ 新增 `cnt_st_backpressure`。兩者由 `enough_free` 互斥 |
| 34 | `cnt_st_iq` 與 `cnt_st_lsq` 定義不對等：前者不管有沒有 uop 要送都計、且 `ds_ready` 是「4 格全空」；後者只計真的被擋 | 記錄為已知不對等，比較兩者時必須註明。灌水的 `st_iq` 仍小於 `st_lsq` |
| 35 | `cnt_st_fetch` 把 flush 後 decode queue 重填的拍算進「前端斷流」，真正來源是誤預測 | 新增 `cnt_st_refill`。**要求：新舊兩個 counter 相加恰好等於舊值**（互斥切分，非新增或排除） |
| 36 | `ci/gate_count.sh` 把 yosys 兩段 `=== module ===` stat 都加總 | 加 `awk '/^=== /{n++} n>=2'`。先前發表的所有 gate/flop 數皆為 2 倍 |
| 37 | 三個 agent 在 `/tmp` 共用 scratch，Agent F 誤刪 Agent D 的工作 | 分配專屬路徑 `/tmp/agent{d,e,f}_prf/`。**CONTRACT 原本只定義 repo 內的所有權，漏了 `/tmp`** |

### §8 stall 歸因原則（新增，強制）

1. **每個 stall cycle 恰好歸因到一個來源，且必須是真正的來源。** 下游回壓不可記在上游。
2. **重構 counter 時必須能證明新舊集合的和相等**，不是「看起來合理」。
3. **counter 的定義必須對等**，否則不可互相比較。
4. **斷言用「精確等於」而非「≥」** —— 只有 `== +5` 抓得到重複計數。

### §9 sweep 方法論（新增）

1. **成本是量化的**：規則 2 的 max-size + mask 讓 `cfg=48` 與 `cfg=64` 成本相同。IQ 只能選 32/64，LDQ/STQ 只能選 16/32。中間值有 IPC 意義、無成本意義。
2. **`full` 訊號可能是協定假象**：用 Little's Law 反推平均佔用，與 `full` 的觸發率對照。本例 LSQ 平均佔用 25% 卻有 22% 時間報滿 → all-or-nothing 協定造成，加 entry 無效。


## v7（2026-09-21 23:50）— ROB 參數化，以及一條新的驗證規則

### 背景：監督者的流程錯誤

PRF sweep 是**正式派任務**給 D/E/F 去參數化的，三方都做到了。
ROB sweep 監督者**沒有派任務，直接自己改 `ifc.vh` 跑**，然後把「不支援契約沒要求過的尺寸」
當成 agent 的缺陷回報。這是不公平的，也是錯誤的流程。

`rob_cnt [6:0]` 能表示 0..127，**對 CONTRACT §0 宣告的 ROB MAX = 64 完全正確**。

### §10 新規則（強制）：`cfg_*` 必須在完整範圍上被驗證

「最大尺寸 + runtime mask」這個模式有一個特有的失效模式：

```
結構建到 MAX=128，但 cfg mask 只開到 64
  → 所有測試通過
  → 逐位等同 MAX=64 的建置
  → 但 64 以上的路徑從來沒被走過
```

實例：`rob_cnt` 的容量不足在 mask=64 時完全無症狀，mask=96 才讓 commit 停擺（IPC 1.391 → 0.072）。
**lint 抓不到**（`[6:0]` 賦值給 `[6:0]` 合法），**yosys 抓不到**，**預設配置的測試抓不到**。

所以：

1. 每個 `cfg_*` 至少要在 **最小值 / 中間值 / MAX** 三點上跑過功能測試
2. 特別注意「容量不足」而非「寬度不符」的錯誤 —— 佔用計數器要能表示 **MAX 本身**（需 `` `W+1 `` 位元）
3. 回歸測試要斷言 **IPC 或 retire 數不因 mask 增大而劣化**

### §11 驗證工具本身會安靜地騙人

本專案至今有 **5 次**是「驗證工具給出錯誤結果」而非「被驗證的程式碼有錯」：

| # | 工具 | 錯誤 |
|---|---|---|
| 1 | `ci/check_contract.sh` | 把 `%Error: Exiting due to N warning(s)` 這行彙總當成實質 error |
| 2 | 同上 | grep 沒剝註解，agent 把規則抄在註解裡就被判違規 |
| 3 | 同上 | 加入 `select -assert-none t:$dlatch` 後，**yosys 回顯的指令文字被自己的 grep 抓到**，七個模組全部誤判 FAIL |
| 4 | `ci/gate_count.sh` | yosys 印兩段 `=== module ===` stat，腳本兩段都加總 → **所有數字 2 倍** |
| 5 | Agent D 的 sweep 腳本 | `verilator --binary --Mdir obj_N -o sim_N`：**相對路徑的 `-o` 會落在 `--Mdir` 裡面**，執行時跑到更早一輪留下的舊 binary → 整輪測試結果不可信 |

**對策（Agent D 提出，納入契約）：**

> 每個測試結果都必須包含一個**會隨條件改變的指紋數字**。
> 舊 binary 或錯誤的建置不會印出對應的值，因此一眼可辨。

實例：
- `T11`：flush 後實際配得出的實體暫存器數 = **32 / 96 / 160 / 224**（隨 `PRF_N` 64/128/192/256）
- `T15`：交付 uop 數 = **20 / 40 / 60 / 80**（隨 `cfg_fetch_width` 1/2/3/4）

「ALL TESTS PASSED」不算指紋 —— 它在任何 binary 上長得都一樣。

### §12 對手寫展開的固定寬度加 elaboration 斷言

手寫展開 `` `W ``=4 條 lane 的邏輯，若 `` `W `` 改變會**安靜算錯**。用不存在的模組強制編譯失敗：

```verilog
generate if (`W != 4) begin : g_assert_W
    ERROR_module_requires_W_eq_4 bad_W();   // 模組不存在 → hierarchy -check 報錯
end endgenerate
```


## v7.1（2026-09-22）— Agent F 提出的三項裁決

### (1) `cfg_*` 的下限改成 `` `W ``

§6 的 all-or-nothing ready 語意（`ready=0` 時一條都不可送出）與 `cfg < `W` 相衝突：
一組 dispatch 最多 `` `W `` 條，若有效容量小於 `` `W ``，`*_full` 永遠拉高、模型不前進。

**裁決**：`cfg_rob/iq/ldq/stq_entries` 的合法下限改為 `` `W ``（=4）。
實作端可夾住（clamp）而非掛掉，使 §10.1 要求的「最小值測試」仍能通過。
`cfg_mshr_entries` 不受影響（MSHR 不是 dispatch 的 all-or-nothing 資源）。

根本解仍是讓 ready 支援 partial accept —— 那同時會解掉 Little's Law 揭露的
「LSQ 平均只用 25% 容量卻有 22% 時間報滿」的門檻假象。列為待辦。

### (2) 跨模組容量不變量：mem-event side FIFO

**`lsu_q` 的 mem-event FIFO 深度 `MQN=32`。在 `fb_take` 與 `ds_valid` 之間同時在飛的
記憶體 uop 數必須 < 32**，否則 push 被丟棄、之後每次 pop 都錯位 →
**所有 load 拿到別人的 cache 事件。這是資料錯誤不是效能誤差，lint 與 yosys 都抓不到。**

目前邊界：`DQ_N=16` + rename/dispatch ≈ 24 < 32，餘裕 8 條。
**Agent D 若加深 decode queue 會靜默失效。**

偵測（Agent F 實作）：
- **T2**：斷言每個 mem uop 的實測延遲 ≥ 它的 base latency（錯位會讓 90 拍的 DRAM load
  拿到 3 拍的 L1-hit 事件而提早完成）
- **T6 負測**：刻意把 fetch→dispatch 拉到 12 級，**斷言 T2 必須報錯**（實測 latviol 222~235）

T6 的存在保證 T2 不是永遠成立的空檢查。**任何不變量的檢查都應附一個負測證明它抓得到。**

根本解：把 6-bit mem event 塞進 `DUOP` 的保留欄位讓它跟著 uop 走，side FIFO 整個消失，
順便省約 6,000 gate。需要 `DUOP_W` 加寬 + Agent D 與 F 同步修改，列為待辦。

### (3) sweep 陷阱：耦合維度會產生假的平坦曲線

`lsq8` 變體（LDQ 8）實測 `cnt_st_mshr = 0` —— 不是 bug，是 **LDQ 先滿，8 個 MSHR 永遠吃不滿**。

> **MLP 的瓶頸是 `min(MSHR_N, LDQ_N)`。掃 MSHR 數量時若不同時放大 LDQ，
> 會量到一條假的平坦曲線，並得出「加 MSHR 沒用」的錯誤結論。**

這與 §9 的「成本是量化的」是同一類 sweep 方法論陷阱：
**掃一個維度之前，先確認它不是被另一個維度夾住的。**

實證：全 DRAM 時 miss 吞吐 0.0844/cyc，緊貼 `8 MSHR / 90 拍 = 0.0889` 的理論上限 ——
MSHR 確實在當 MLP 的閘門，前提是 LDQ 夠大。


## v7.2（2026-09-22）— 「填滿容量」不等於「填滿容量且有競爭」

### §13 資源競爭測試

CONTRACT §10 要求「每個 `cfg_*` 在最小/中間/MAX 三點上跑功能測試」。
**這不夠。** Agent E 找到的 `be_eu` completion wheel 溢位證明了這一點：

```
mode 7（獨立 lat=15）  能把 in-flight 堆到 64   → 不觸發
mode 9（長延遲 ALU + 大量 load，製造 lsu_done 競爭）→ 觸發
```

根因是共享資源（writeback port）的競爭讓項目被迫「重新排程」，殘留時間超過
uop 本身的延遲，最終塞爆固定容量的結構並**靜默丟棄**。

> **測試必須在目標尺寸下同時製造資源競爭，不能只是把結構填滿。**

### §14 結構性不可能 > 容量足夠

`be_eu` 原本的修法選項是「把 wheel 加大到 ROB_N」。Agent E 改成
**robidx 定址的 in-flight pool**：每條 issue 出去的 uop 用自己的 `robidx` 當 slot 編號。

不變式：ROB entry 在 commit 前不會重配、每條 uop 只 issue 一次
→ 每條 in-flight uop 都有專屬 slot
→ **不需要配置邏輯、不需要空槽搜尋、沒有任何路徑會丟件**

結果：狀態量**變小** 165 bit（wheel 的 `rob` 欄位、`mem_dv` 表、mem FIFO 全部消失），
gate 只 +10.6%，而且連 ROB=64 的 IPC 都從 1.391 → 1.439（+3.5%）——
舊的重新排程路徑本來就在偷吃效能。

> **能讓失效在結構上不可能發生時，不要只是把容量加大。**


## v8（2026-09-22）— ready 改成 partial accept

### 動機（實測，非推測）

診斷 counter 在真 CoreMark trace 上量到：

```
lsq_full 總拍數        118,331
  真滿（一格都沒有）        30    ←  0.03%
  假滿（還有 1~3 格）  118,301    ← 99.97%
  真+假 == cnt_st_lsq           ✓ 守恆

LDQ 平均佔用 7.17/16 (45%)   STQ 平均佔用 2.24/16 (14%)
```

**LSQ 報滿的拍次裡 99.97% 是 all-or-nothing 協定造成的假 stall**，佔總 cycle 的 23.7%。
加 LDQ/STQ entry 只能救那 30 拍。

### §6 v2 協定（取代原本的 all-or-nothing）

```verilog
// 舊：單 bit，all-or-nothing
output wire                  xx_ready;      // 0 → 一條都不可送

// 新：可接受的條數
output wire [2:0]            xx_nready;     // 0..`W，「我這拍能收前 N 條」
```

**規則：**

1. **依序接受**：下游接受的一定是 bundle 裡**最前面的 `n` 條**，不可跳號
   （跳號會破壞程式順序，而 ROB/LSQ 的配置依賴它）。
2. **`nready` 不得依賴 `valid`** —— 只能看下游自己的狀態（空位數）。
   這是避免 valid↔ready 組合迴圈的硬性要求（Agent D 在 v1 已指出）。
3. **上游必須保留未被接受的 uop，依序重送**，且 `valid` 不得因 `nready` 變小而撤回。
4. 送出數 = `min(popcount(valid_in_order), nready)`。

### §8 v2 stall 歸因（partial accept 下的修訂）

原本「每個 stall cycle 恰好一個來源」在 partial accept 下不夠用 ——
一拍可能「收了 2 條、擋了 2 條」，既不是完全 stall 也不是完全通過。

**新定義（兩個互補的量，不可互相取代）：**

```verilog
output wire [47:0] cnt_st_XX;        // nready == 0 且有東西要送 → 完全停擺的拍數
output wire [47:0] cnt_lost_XX;      // Σ (想送的條數 − 實際送出的條數) → 損失的 uop-slot
```

- `cnt_st_XX` 保留原本的「拍數」語意，仍須互斥、可加總
- `cnt_lost_XX` 是**吞吐損失**的直接度量，單位是 uop 不是 cycle
- 兩者都要報。只看 `cnt_st` 會低估 partial 的代價，只看 `cnt_lost` 會看不出完全停擺

### 預期效果（待驗證）

- 消除 LSQ 的 118,301 拍假 stall（23.7% 的總 cycle）
- 消除 `cnt_st_iq` 裡「剩 1~3 格也算滿」的同類灌水
- **`cfg_*` 的下限可以回到 1**（v7.1 因 all-or-nothing 被迫改成 `` `W ``）
- ROB 的同類門檻效應

### 風險

- **組合迴圈**：`nready` 依賴 `valid` 會立刻形成迴圈。規則 2 是硬性的。
- **partial 之後的順序**：上游保留未送出的 uop 時，必須維持程式順序且不可重排。
- **bundle 內相依**：rename 的 bundle 內 RAW/WAW 旁路原本假設整組一起送，
  partial 之後前半送出、後半保留，下一拍的旁路來源改變 —— 這是最容易出錯的地方。

## v8.1（2026-09-22）— Agent F 的兩項糾正

### (1) `lsq_nfree = min(ld_free, st_free, `W)` 不是保守值，是**緊界**

監督者原本把它描述成「簡單但 3 load + 1 store 時會低估」。**錯。**

一條記憶體 uop 可能要 LDQ（load）、要 STQ（store）、或 **兩者都要（AMO）**。
所以長度 N 的前綴最壞會吃掉 N 個 LDQ **且** N 個 STQ。
任何 `N > min(ld_free, st_free)` 的承諾都能被「N 條全 load」或「N 條全 store」打爆
→ 違反規則 1（依序接受不可跳號）。

**在「不得窺探 bundle 組成」的前提下，(a) 已經是可能的最大值。**

要更精確只能讓 rename 額外送出兩個 `` `W ``-bit 的**類別遮罩**
（每 lane「是不是 load」「是不是 store」），且**獨立於 `valid`** ——
那樣 `nfree` 依賴「組成」而非「有效」，規則 2 仍成立、無迴圈。
實測收益：cfg=16 時只多救 32 個 uop-slot（不值得），
但 **ROB=128 時 `lost` 跳到 4,680，(b) 可多救 2,267（≥48%）** —— 屆時值得做。

### (2) STQ 佔用偏低有一部分是**模型簡化**，不是 workload 性質

監督者從 LDQ 45% / STQ 14% 的實測推論「16/16 對稱配置浪費，STQ 8 格就夠」。
**這個推論不安全。**

`lsu_q` 的 store 一律當 store-buffer hit（`ST_LAT=2`、不配 MSHR），
所以 STQ entry 只佔 2~3 拍就排掉，**自然偏空**。

> **掃 STQ 大小之前，必須先讓 store 也吃 `MEM_LEVEL`（write-allocate），
> 否則量到的是模型簡化的產物而不是設計取捨。**

這與 §7.1(3)「耦合維度會產生假的平坦曲線」是同一類陷阱的另一種形式：
**曲線平坦可能是因為那個維度被模型簡化架空了。**

## v8.2（2026-09-22）— Agent D 的三項發現

### (1) 規則 1 讓 bundle 旁路問題自動消失

監督者把「bundle 內 RAW/WAW 旁路」標為 partial accept 最難的部分。**它是免費的。**

規則 1（依序接受、不可跳號）保證 `lane j 被接受 ⇒ 所有 i<j 也被接受`。
而旁路只會「lane j 旁路自 lane i<j」。
→ **每個被接受的 lane，它的旁路來源必定也被接受**
→ 旁路網路對前綴**逐位不變**，一行都不用改。

要改的只有提交那一層：`w_j = acc[j] & nd_j`（只有真送出的 lane 才配暫存器、才寫 RAT）。

> **規則 1 原本是為程式順序訂的，結果順便解決了旁路問題。
> 一條為 A 訂的約束解掉 B，通常表示 A 是對的抽象。**

補充兩個正確做法：
- **保留的 uop 必須重新查 RAT，不可跨拍快取。** 它們根本沒離開 decode queue；
  下一拍佇列壓縮、重新查 `rat_q`（此時已含上一拍被接受 lane 的寫入）。
  「沿用上一拍算好的值」在相依於同 bundle 時碰巧一致，相依於更早的東西時會錯。
- **不可替未送出的 lane 預配實體暫存器。** freelist 位元只在真送出時才清，
  否則下一拍重配 → 洩漏。

### (2) 跨模組不變量：`n_xfer` 三方一致

```
fe:   出隊 = min(n_pres, de_nready)
rn:   配置 = min(want, n_free, rn_nready)
ds:   取用 = min(popcount(rn_valid), rn_nready)
```

三式因 `rn_valid = thermometer(min(want, n_free))` 且 `de_nready = min(n_free, rn_nready)` 恆等。

> **若 `be_dispatch` 取得比 `min(popcount(rn_valid), rn_nready)` 少
> （例如自己再濾掉 wrong-path uop），rename 就會替沒被收下的 uop 配了實體暫存器
> → 下一拍重配 → 實體暫存器洩漏。**

規則 4 必須被下游**嚴格**遵守，不可「少收一點比較安全」。

### (3) 規則 2 的必然代價：容量低報

`de_nready` 不准看 `de_duop`（規則 2），所以 rename 只能假設「每條 uop 都要配暫存器」。
進來的是 store/branch 時會**低報容量**。

這是協定的代價不是 bug —— 消除它就得讓 `nready` 看 `valid`，立刻成組合迴圈。

**診斷方式**：若 `cnt_lost_rename` 在 freelist 沒滿時仍很大，就是這個保守性在作用。

（與 Agent F 的 `lsq_nfree` 緊界證明是同一個現象的兩種表現：
**看不到 bundle 組成時，保守是唯一安全的選擇，代價可量測但不可消除。**）

## v9（2026-09-22）— wrong-path shadow

### §15 `shadow_off` 改成相對偏移（Agent H 回報的格式上限）

`shadow_off` 是 `uint32_t`。v8 以前存**絕對檔案偏移**，但 50M trace 的 correct-path 區
就佔 800 MB，只剩 **K ≈ 349** 的餘裕；**200M 指令的 trace 在任何 K 下都會溢位**。

**裁決（修正）**：**維持絕對偏移，列為已知上限。**

監督者原本裁決「改成相對」並寫進了 `bstf.h` 的註解，**但沒有派任何人實作** ——
H 產生的資料與 G 的 reader 都仍是絕對偏移。**規格只改在紙上，資料沒動。**
若當時有人照註解實作，兩端會立刻對不起來。

這是本專案第 N 次「文件與實作分岔」，而且是監督者自己造成的。
**改規格必須同時派實作，否則就只記為已知限制。**

目前處置：
- `bstf.h` 的註解改回描述**實際**格式（絕對偏移）
- 上限（50M trace 時 K≈349，200M 任何 K 都溢位）記為已知限制
- `bstf_gen` 已加硬性失敗而非靜默 wrap —— 那是正確處置
- 真要修時，改成相對可買 5× 餘裕，但需同時改 H 的產生端與 G 的 reader

（Agent H 原本加了硬性失敗而非靜默 wrap —— **那是正確的處置**：
格式上限應該炸出來，不該悄悄算錯。）

### §16 wrong-path 的已知近似（讀結果時必須知道）

Agent H 的 shadow 由一個自寫的 RV64IMC+Zba/Zbb/Zbs 執行引擎產生
（commit log 依定義只含 retire 的指令，wrong path 必須**重新執行**才有）。

**可信度依據**：同一引擎對完整 correct path 逐條 lockstep，
**50,010,138 條指令中 0 個未支援編碼、0 個目的值錯、0 個有效位址錯、0 個 next-PC 錯**
（7 條 CSR/SYSTEM 刻意不模擬）。lockstep 本身也是**找出引擎 bug 的工具**。

**三項殘留近似：**

1. **139,926 / 625,800（22%）的 shadow 來自「因 fetch 寬度而結束」的 block**，
   所以種子暫存器狀態比誤預測的那條 branch 晚 1~3 條指令。
2. **9,861 個 target-only 誤預測被模型化成 BTB miss（順序 fetch）**，
   而非「BTB 有舊 entry 指向別處」。**舊目標無法從 trace 回推。**
3. **`jalr`/`ret` 的間接跳躍佔 760 個（0.12%）**，用 dummy uop 退化處理。
   這個比例遠低於預期的 5%，因為**直接分支的 target-only 誤預測仍可展開**（BTB miss → 順序）。

### §17 padding 的檢查必須看位置不看內容

Agent H 的第一版用「內容」判斷 shadow 區的 padding，**產生 995 個假陽性** ——
因為真實 wrong-path 上的 `c.nop` 與 pad 記錄**逐位元相同**。

> **任何「用內容辨識結構」的檢查都要先問：這個內容會不會合法出現？**

### §18 分離的 overlay 必須帶「真實執行順序」的索引

`.mem.wp.txt` 若單獨餵給 cache 模擬器，**等於完全沒有模擬污染** —— 而污染正是要它的理由。

Agent H 加了 `after_mem_idx`（該筆 wrong-path 存取之前有幾筆 correct-path `.mem`），
讓下游能把兩股串流合併回真實順序，同時保持兩個 overlay 檔分離。

另：**存取到未映射頁面的 6,803 筆不輸出** —— 會 fault 的存取根本到不了 cache，
輸出垃圾位址會污染 cache 模擬器的 tag array。

### §19 `.fe.wp` 的粒度 —— 監督者從未定義，導致兩端假設不同

整合時 Agent G 的 reader 報：

```
.fe.wp: size 155,810 != expected 623,240   （差 4 倍）
```

兩邊的假設：

| | 粒度 | 算式 |
|---|---|---|
| **Agent A 產生的** | **每個 wrong-path fetch block 一 byte** | 15,581 誤預測 × D=10 = **155,810** |
| **Agent G 的 reader** | 每條 wrong-path 指令一 byte | `shadow_bytes / rec_bytes` = **623,240** |

**兩邊都自洽，合起來不對** —— 因為 CONTRACT §5（v4）定義 `.fe.wp` 時只寫了
「第 k 次誤預測對應 byte 區間 `[k*D, (k+1)*D)`」，**沒有說 D 的單位是 block 還是指令**。

**裁決：`.fe.wp` 的粒度是 fetch block**，與 `.fe` 一致（`.fe` 也是每 block 一 byte）。
理由：它存的是 `FE_*` 事件（bubble 數、uBTB 命中、override），那些都是 **block 層級**的屬性，
不是指令層級的。

```
.fe      每個 correct-path fetch block 一 byte
.fe.wp   每個 wrong-path   fetch block 一 byte，第 k 次誤預測 = [k*D, (k+1)*D)
.mem     每個 data 存取一 byte
.mem.wp  每筆 wrong-path data 存取一列（文字，含 after_mem_idx）
base     每條指令 16 byte
```

**四種 overlay，三種不同的粒度。** 這是這次缺陷的根本成因 ——
而它到整合才現形，因為 `shadow_off` 在此之前一直是 0，`.fe.wp` 從未被讀取過。

> **每個 overlay 的粒度必須在契約裡明寫，不能靠「跟另一個一樣」推論。**

## §20 參數化必須做到「單一來源」，per-config 手改檔不算參數化

v4 修正：`RUOP_S1/S2/D` 從第一個 commit 起就是寫死的 6 bit（`21:16` / `14:9` /
`7:2`），而 rename 端寫入的是 `` `PRF_W `` 寬的值。`PRF_N > 64` 時 Verilog 把高位
**靜默截斷**，實體暫存器編號發生別名，rename→IQ→EU→ROB 的整條相依鏈全錯，
表現是 LSQ 永久 full、IPC 掉到 0.004。

三層驗證全部沒抓到：

| 驗證 | 為什麼放過 |
|---|---|
| `verilator --lint-only -Wall` | 位元寬度不匹配的隱式截斷不是 warning |
| free list 自測 | 只檢查可配置數 = `PRF_N - 32`，不經過 RUOP 編碼 |
| 模組級 TB | Agent E 在 `/tmp/agente_prf/prf7,prf8/` **手改了 per-config 的 ifc.vh**（`RUOP_W 48`、`RUOP_S1 24:18`），所以它的 TB 是對的 —— 但那份重佈局沒進 repo |

**後果**：dd31c15 發布的 PRF 64/96/128/192/256 表來自 Agent E 的 scratch，不是整合
模型跑 CoreMark 的結果。整合模型在 repo 裡從來沒有能力表示編號 ≥ 64 的實體暫存器。

**裁決**：
1. 多位元欄位一律用 `` `FIELD +: 寬度 `` 存取，欄位基底由參數算出，不得寫 `[hi:lo]`。
2. 參數 sweep 的交付物是**同一份原始碼**跑不同 `define`。任何「我改了一份 config
   專用的檔案來跑」都不是參數化 —— 那份改動如果不在 owner 的檔案裡，它就不存在。
3. 模組級 TB 通過不代表整合通過。跨模組的編碼契約只有整合跑真 trace 才會驗到。
