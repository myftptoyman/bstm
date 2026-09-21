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
| 結構 | MAX | 索引寬度 |
|---|---|---|
| ROB | 64 | 6 bit |
| IQ | 32 | 5 bit |
| LDQ / STQ | 16 / 16 | 4 bit |
| MSHR | 8 | 3 bit |
| 實體暫存器 PRF | 64 | 6 bit |
| fetch / issue / commit width | 4 | 3 bit |

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
cfg_rob_entries   [6:0]   1..64
cfg_iq_entries    [5:0]   1..32
cfg_ldq_entries   [4:0]   1..16
cfg_stq_entries   [4:0]   1..16
cfg_mshr_entries  [3:0]   1..8
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
