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
