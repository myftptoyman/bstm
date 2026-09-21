# BSTM OOO 後段（Agent E）— `model/ooo/backend/`

4-issue out-of-order 的 DS / IS / EX-WB / CM 四級，Verilog-2005。
介面依 `model/ooo/common/ifc.vh`（v3，`RUOP_W`=40、`RUOP_ARFD/ARFDV`、`rob_full` 回授），
埠列與 `model/ooo/top.v` 一致，未增刪任何埠。

| 檔案 | 模組 | 級 | 功能 |
|---|---|---|---|
| `be_dispatch.v` | `be_dispatch` | DS | 收 rename 的 RUOP、配置 ROB entry、stall 歸因 |
| `be_iq.v` | `be_iq` | IS | issue queue：bit-matrix wakeup + age-matrix oldest-first select |
| `be_eu.v` | `be_eu` | EX/WB | 純延遲倒數 + prf_ready scoreboard + LSU 轉手 |
| `be_rob.v` | `be_rob` | CM | ROB、in-order commit、flush、全域 counter |

---

## 1. `be_dispatch`

* **all-or-nothing**（ifc.vh v2 的 ready 語意）：`rn_ready = ~rob_full & ~lsq_full & ~mshr_full & ds_ready & ~flush`，
  不依賴 `rn_valid`，所以不會跟 rename 形成組合迴路。
* ROB entry 用 round-robin tail 指標配置，每條有效 uop 推進一格，
  折返點是 `cfg_rob_entries`（不需要 2 的冪次，用 `>= cfg` 比較折返）。
  `ds_robidx` 每 lane 6 bit，無效 lane 的欄位維持前一條的值（ROB 端以 `ds_valid` 篩選）。
* `flush` 當拍 `ds_valid` 全 0、tail 歸零，與 `be_rob` 的 head 歸零對齊。
* v3 起 `rob_full`（來自 `be_rob`）與 `mshr_full`（來自 `lsu_q`）都是真回授，
  本模組不再自己估計 ROB 佔用，只保留 tail 指標。
* counter：`cnt_st_rob`（有 uop 可送但 `rob_full`）、`cnt_st_lsq`（同上但卡在 `lsq_full`），
  歸因順序 ROB > LSQ，一拍只記一個原因；MSHR 滿會 stall 但由 `lsu_q` 的 `cnt_st_mshr` 計，
  本模組不重複計。
* 註：counter 的計數條件含 `|rn_valid`，前提是 rename 遵守 valid/ready 協定
  （`ready=0` 時 `valid` 不得撤回）。若上游把 valid 收回去，stall 就不會被記到。

## 2. `be_iq`（最關鍵的模組）

### 2.1 wakeup：bit-matrix，沒有任何 tag 比較器

每個 entry 存一條 `PRF_N`(64) 位元的「尚未就緒來源」遮罩 `e_pend[i]`：

```
配置時   e_pend[i] = (onehot(s1) | onehot(s2)) & ~eff_ready_at_dispatch
每拍     e_pend[i] <= e_pend[i] & ~eff_ready            // 就是整條 AND
就緒判斷 rdy[i] = e_v[i] & ((e_pend[i] & ~eff_ready) == 0)
```

`onehot()` 是用 64 個常數比較展開的解碼器，不是 shift。
整個 wakeup 網路只有 AND / OR-reduce，沒有 `==` tag 比對，
在 bit-sliced 後端就是每條 entry 幾個 word 的 `and` + `or`（PLAN §6.5 的建議 2）。
`e_pend` 既可組合判斷、也每拍收斂，遮罩會隨時間變稀疏。

### 2.2 `eff_ready`：解決「dispatch 到 issue 之間 prf_ready 是舊值」的問題

`prf_ready` 由 `be_eu` 維護，`be_eu` 看不到 dispatch（埠列沒有），
所以一個剛被 rename 配出去的 phys reg，在 producer 真的 issue 之前，
`prf_ready` 還是上一輪留下的 1。IQ 端用一條 `wait_m` 遮罩補掉這個窗口：

```
wait_m[p] : p 已被 dispatch 當 dst 配出去、但 producer 還沒 issue
          set  = 本拍 dispatch 的 dst（含同拍 lane0->lane3 的相依）
          clr  = 本拍 issue 出去的 uop 的 dst
eff_ready = prf_ready & ~wait_m
```

producer issue 之後 `be_eu` 會把 `prf_ready[p]` 清掉（lat>=2 與 mem），
所以從 issue 到 writeback 這段時間靠 `prf_ready` 本身擋住，語意接得起來；
lat<=1 的 uop 刻意不清（見 3.2），於是 `wait_m` 清掉的同拍就能被喚醒 → back-to-back。

### 2.3 select：age matrix，oldest-first

`e_age[i][j] = 1` 代表 entry j 比 entry i 老。配置時整列寫入「目前還在 IQ 的 entry 集合」，
issue 出去時把該欄清掉（`e_age[i] <= e_age[i] & ~sel_m`），
同拍配置的 4 條之間用 lane 順序定序（lane0 最老）。

每一輪：`oldest[i] = elig[i] & ((e_age[i] & elig) == 0)`，
因為 age 是全序關係，結果保證是 one-hot，不會有 starvation。
挑完把它從 `elig` 拿掉再挑下一輪，共 `cfg_issue_width` 輪。

**資源限制**：`UC_DIV` 同拍最多 1 條、`UC_MUL` 最多 2 條，
用「某類別額滿就把該類別整批從 `elig` 拿掉」實作，
所以額滿時會自動跳過去挑下一條更老的其他類別 uop（不是直接停住）。

### 2.4 其他

* `iss_*` 是**組合輸出**（select 當拍就送到 EU）。這是 back-to-back 能成立的前提，
  代價是 `prf_ready(reg) -> IQ select -> EU` 這條組合路徑比較長。
  時序模型不在意閘延遲，bit-sliced 後端只在意 cell 數，所以這個取捨是划算的。
* `ds_ready = (有 W 個空槽)`，不依賴 `ds_valid` → 無組合迴路，符合 all-or-nothing 語意。
* `cnt_st_iq` 計的是 `ds_ready == 0` 的 cycle 數（IQ 收不下一整組的拍數）。
* **空槽搜尋與「讀 `ds_*` 的部分」刻意拆成兩個 `always @*`**：
  合在一起的話 verilator 會判定 `ds_valid -> a_ok -> ds_ready -> be_dispatch -> ds_valid`
  是組合迴路（UNOPTFLAT），實際上 `a_ok` 只依賴 IQ 自己的狀態。
  整合到 `top.v` 時這個拆法是必要的，請勿合併。

## 3. `be_eu`

### 3.1 in-flight pool：用 robidx 當 slot（v4，原本是 completion wheel）

每條 issue 出去的 uop **直接拿自己的 `robidx` 當 slot 編號**：

```
slot[robidx] = { v, wt(等 LSU), dv, cnt(`LAT_W), prf(`PRF_W) }
pool 大小 = `ROB_N（由巨集推導，ROB 加大自動跟上）
```

ROB entry 在 commit 之前不會被重新配置、每條 uop 只會被 issue 一次，
所以「兩條 in-flight uop 撞同一個 slot」**在結構上不可能發生**：
不需要配置邏輯、不需要空槽搜尋，**也不可能溢位丟件**。

* `cnt` 倒數到 0 = 可寫回；搶不到 writeback port 就停在 0 等下一拍，
  不會被丟掉、也不需要重新排程。
* `lat<=1` 的 uop 與**本拍回來的 `lsu_done`** 走 bypass 直接參加本拍競爭，
  所以 lat=1 的相依者下一拍就能 issue（back-to-back 與 v3 完全一致）。
* writeback port 用**旋轉優先權**挑最多 W 條，不會餓死。
* `cnt` 設定值：`lat>=2 -> lat-2`，`lat<=1 -> 0`（配合 bypass，
  使「issue 於 T、writeback 於 T+lat」與舊版逐拍相同）。

**為什麼要換掉 completion wheel**：舊版是 4 lane × 16 槽的環形輪，
容量固定 64。`lsu_done` 會搶走該 lane 的 writeback port，輸掉的輪子項目
要「重新排程」到後面的空槽 —— 殘留時間因此會超過 uop 本身的延遲。
ROB mask 開到 96/128 之後 in-flight 數變多、`lsu_done` 競爭變密，
lane 的 16 槽會被塞滿，`find_free` 找不到空槽就**把 uop 丟掉**，
那條 uop 永遠不會 writeback → ROB head 永久卡死 → commit 停擺。
詳見 §8。

### 3.2 `prf_ready` scoreboard（clear / set 優先序）

```
rst / flush : 全部設 1（管線清空，所有暫存器視為就緒）
clear       : issue 且 dv 且（mem 或 lat>=2）的 dst
set         : 本拍仲裁贏的 writeback（dv=1 才 set）
prf_ready <= (prf_ready | set) & ~clear      // ***clear 優先***
```

* **lat<=1 的 uop 故意不 clear**：它的 clear 與 set 會落在同一個時鐘邊緣，
  不 clear 才能讓「lat=1 的結果在 writeback 當拍就是 ready」，相依者下一拍即可 issue。
* clear 優先的語意是「新配置蓋掉舊結果」。rename 只會在 old phys 被 commit 之後才重配，
  所以同 bit 同拍 set+clear 在正常流程下不會發生；規則寫死只是為了讓行為可決定。
* **speculative wakeup（load-hit 預測喚醒）沒有做**，也沒有 replay 機制：
  load 一律等 `lsu_done_*` 回來才叫醒相依者，等於永遠假設 load 預測失敗。
  這會低估 IPC；要做的話得在 IQ 加 replay 佇列，屬於下一階段。

### 3.3 記憶體 uop

`UC_LOAD` / `UC_STORE` / `UC_AMO` 不在 EU 完成：slot 標成 `wt`（等 LSU），
同時在 `rq_pend[robidx]` 記一筆待送的請求。每拍在 `lsu_ready` 時
用同一套旋轉優先權挑最多 W 條送 `lsu_req_*`（issue 到送出 1 拍延遲）。
`rq_pend` 一樣由 robidx 定址，所以**請求也不可能被丟掉**
（v3 用的是 16 深的 FIFO，滿了會丟）。

`lsu_done_*` 回來時清掉該 slot 的 `wt`，之後就跟一般 uop 一樣排隊寫回。
**不再需要 `mem_dv` 表、也不用 `lsu_done_prf`** —— EU 在 issue 當拍就把
dst 存進自己的 slot 了，用自己的比相信 LSU 回填的更穩。

因為 `lsu_done` 只是「把 slot 標成可寫回」而不是直接搶 writeback port，
v3 那個「輸掉的項目要重新排程」的路徑整條消失了。

## 4. `be_rob`

* 64 entry 環形，欄位 `v / done / wrongpath / dv / arf(5bit) / d(6bit)` 全部是 packed vector，
  只用常數索引展開成 mux 鏈，不用 unpacked 陣列 → 不可能推出 `$mem`。
* commit：從 head 掃最多 `cfg_commit_width` 條，必須 `v & done`，遇到第一條沒完成就停。
* **flush**：掃到一條 `wrongpath` 的 uop 就不 retire 它，直接拉 `flush` + `flush_robidx`，
  同拍整個 ROB 清空、head 歸零。排在它前面的正確路徑 uop 本拍照常 commit。
* **`rob_full`（v3）**：`rob_cnt + W > cfg_rob_entries` 就拉高，直接回壓 `be_dispatch`。
  用的是本拍開始時的佔用（不含本拍的 commit），所以滿的邊緣會保守 1 拍，這跟真硬體一致。
* commit 介面 v3：
  - `cmt_prf` = 這條 uop **寫入**的 phys reg（`RUOP_D` 原樣輸出），舊 mapping 由 rename 的 cRAT 釋放。
  - `cmt_arf` = `RUOP_ARFD`（架構目的暫存器）原樣輸出。
  - `cmt_dv` = **`RUOP_DV`**（有沒有配到實體暫存器），**不是** `RUOP_ARFDV`。
    `dst == x0` 的 uop 是 `ARFDV=1 / DV=0`，用 ARFDV 會白佔一個 commit lane 的釋放名額、
    把 counter 語意弄髒；rename 端另外以 `cmt_arf != 0` 過濾（見 `frontend/README.md §3.2`）。
    因此 `RUOP_ARFDV` 在本模組沒有存、也沒有用。

### counter 語意（全部 48-bit 純累加，rst 歸零）

| counter | 語意 |
|---|---|
| `cnt_cycles` | rst 解除後每拍 +1（全域時間基準，只在本模組產生） |
| `cnt_retired` | 正確路徑 commit 的 uop 數（wrongpath 那條不算） |
| `cnt_wrongpath` | flush 當拍 ROB 裡剩下的有效 uop 數（含觸發的那條） |
| `cnt_rob_occ_sum` | 每拍累加「本拍開始時的佔用數」；`/cnt_cycles` = 平均佔用 |
| `cnt_st_rob` / `cnt_st_lsq` | 見 1.（v3 起 `cnt_st_rob` 是真的 ROB 滿拍數） |
| `cnt_st_iq` | `ds_ready==0` 的拍數 |

`cnt_mispred` 依監督者裁決由 `fe_front` 計，本模組不重複計。

---

## 5. 驗收結果

```
yosys: read_verilog -I. backend/X.v; hierarchy -check -top X; proc; opt; check -assert; stat
```

| 模組 | cells | memories | 備註 |
|---|---|---|---|
| `be_dispatch` | 37 | 0 | add/mux/ge/sdff(e)（v3 拿掉佔用估計後變小） |
| `be_iq` | 7852 | 0 | mux 4116、eq 1419、and 686（age+pend matrix） |
| `be_eu` | 6438 | 0 | mux 3953、eq 1335；**`$shift`/`$shiftx` = 0**（v3 改常數索引展開，+1852 cells） |
| `be_rob` | 4674 | 0 | mux、eq、logic_and（v3 多存 5-bit arf + rob_full） |

四個模組**都沒有** `$shift` / `$shiftx` / `$mul` —— 所有變數索引都展開成常數比較的 mux 鏈。
`be_eu` 的 16 槽空位搜尋改用 4 段常數 part-select 的右旋（手寫 barrel，純 mux），
不是 `>>`，所以不會生位移 cell。

四個模組都是 `check -assert` 0 problems，`Number of memories: 0`，
stat 沒有 `$__RAMGEM_ASYNC_`、沒有 `$mem`、沒有 `$dlatch`。
yosys 只有 4 則 "Replacing memory \e_xxx with list of registers"（`be_iq`），
那正是我們要的結果（陣列全常數索引 → 直接展成暫存器）。

`verilator --lint-only -Wall -I. backend/*.v` → exit 0，零 warning
（四個檔都加了 `lint_off MULTITOP`，因為一次 lint 四個平行模組本來就沒有唯一 top）。

`ci/check_contract.sh` 全綠。唯一一則 WARN 是 `be_rob` 的 `reg [14:0] ent;`，
那是 ROB entry 的讀出 bundle `{v,done,wp,dv,arf[4:0],d[5:0]}`（一次 64:1 mux 讀完整筆），
不是寬算術欄位。

### 自測（verilator `--binary`，testbench 放在 `/tmp/agente_prf/tb/`，不屬於交付物）

把四個模組照 `top.v` 接起來，配假 rename（總是給滿 4 條）與假 LSU（3 拍回 done），
`cfg_*` 全開，跑 500 拍：

| 測試 | 結果 | 期望 |
|---|---|---|
| 獨立 ALU lat=1 | IPC 3.97 | ~4（issue width） |
| lat=1 相依鏈 | IPC 0.99 | **1.0 → back-to-back wakeup 正確** |
| lat=3 相依鏈 | IPC 0.33 | 1/3 |
| 混合（load/mul/div 相依鏈） | IPC 0.31 | — |
| 獨立 `UC_DIV` lat=8 | IPC 0.98 | ~1（DIV 只有 1 個） |
| 獨立 `UC_MUL` lat=3 | IPC 1.98 | ~2（MUL 只有 2 個） |
| 每 97 條一條 wrongpath | IPC 3.41、`cnt_wrongpath`=201 | flush 會發、會恢復、不卡死 |
| 獨立 `UC_FPU` lat=15 | IPC 3.44、`cnt_st_rob`=54、佔用 59.4 | **v3 前這組 500 拍 retired=0 全卡死**，`rob_full` 修好了 |
| `mshr_full` 50% duty | IPC 1.98（= 3.97 的一半） | MSHR 回壓確實擋得住 dispatch |
| `cmt_arf` 端到端 | 所有模式 `arf_bad`=0 | TB 把 `RUOP_ARFD` 設成 `RUOP_D[4:0]`，逐條 commit 比對 |

---

## 6. 已知限制

> v2/v3 回報的兩個阻斷級問題（ROB 沒有 backpressure、`cmt_arf` 無來源）
> 以及 `mshr_full` 沒人消費，監督者都已修進 `top.v`/`ifc.vh` v3，本節只剩下真正的簡化。

### 6.1 簡化清單

1. **partial dispatch 不支援**：`rn_ready` / `ds_ready` 都是 all-or-nothing（照 ifc.vh v2），
   IQ 要有 4 個空槽才收，所以 IQ 剩 1~3 格時會浪費一點吞吐。
2. **speculative wakeup / replay 沒做**（見 3.2），load 相依者一律等 `lsu_done`。
3. **`UC_SERIALIZE` / `UC_FENCE` / `UC_CSR` 沒有特別處理**，一律當成一般延遲 uop。
   `RUOP_SERIALIZE`、`RUOP_MEMSTORE` 兩個欄位後段目前沒用到。
4. ~~EU 每 lane 每拍最多完成 1 條~~ —— v4 改成 pool + 旋轉優先權後，
   writeback port 由全部 in-flight uop 共用，沒有 per-lane 限制了。
5. ~~mem request queue 16 深，滿了會丟請求~~ —— v4 改成 `rq_pend[robidx]` 位元，
   結構上不可能溢位（見 3.3）。**v3 的這兩條「極難觸發」在 ROB 開到 96 之後
   都變成必然觸發**，教訓寫在 §8。
6. **flush 後 `prf_ready` 全設 1**。管線整個清空，沒有 in-flight 的 producer，
   這是最簡單且不會卡死的選擇；rename 端重建 RAT 後配出來的 phys reg
   會由 IQ 的 `wait_m` 重新擋住，語意仍然正確。
7. ~~`be_eu` 的 `$shift`/`$shiftx`~~ —— v3 已全部改成常數索引展開，四個模組皆為 0。
8. `be_rob` 的 `rob_full` 用本拍開始時的佔用判斷，ROB 剛好卡在滿的邊緣時會多 stall 1 拍。
9. **`cfg_*` 的有效下限是 `W` 不是 1**：ready 是 all-or-nothing（ifc v2），
   dispatch 要有 W 個空位才送得出去，所以 `cfg_rob_entries < W`（同理 `cfg_iq_entries < W`）
   會直接停擺。實測 `cfg_rob_entries` = 2/3 → retire 0，= 4 → 正常。
   CONTRACT §3 寫的是 `1..64`，**實際可用範圍是 `W..ROB_N`**，建議更正契約或改成支援 partial dispatch。

## 7. PRF sweep（`PRF_W` = 6 / 7 / 8 → `PRF_N` = 64 / 128 / 256）

### 7.1 參數化修正（這次補的）

| 位置 | 原本 | 問題 | 改法 |
|---|---|---|---|
| `be_rob` `rd_ent` bundle | 硬編 `ent[14]/[13]/[12]/[11]/[10:6]`、`15'd0` | `PRF_W` 一變欄位就錯位（而且是**靜默**錯位） | 全部改成 `` ent[3+ARFD_W+`PRF_W] `` 這種相對式 |
| `be_rob` 取 `RUOP_D` | ``ds_ruop[k*`RUOP_W + 2 +: `PRF_W]`` | 硬編偏移 2，RUOP 一重新佈局就靜默壞掉 | per-lane wire + ``duop[k][`RUOP_D]`` |
| `be_rob` 取 `RUOP_ARFD` | ``ds_ruop[k*`RUOP_W + 32 +: 5]`` | 同上（硬編 32） | ``duop[k][`RUOP_ARFD]`` |
| `be_rob` ARFD 欄位寬 | 散落的 `5` | Verilog-2005 無法從 range 巨集取寬度 | `localparam ARFD_W = 5`（ifc 若加寬要同步改） |

`be_dispatch` / `be_iq` / `be_eu` 本來就全走巨集，這次沒有改動（`be_eu` 的 `ohrob()` 是監督者先補的）。

### 7.2 驗證（三件事都做了）

1. **lint**：三種 `PRF_W` 下 `verilator --lint-only -Wall -I. backend/*.v` 都 **exit 0、0 warning**。
2. **`ci/check_contract.sh`**：三種都 **PASS=20 FAIL=0**（yosys 合成、0 problems、無 latch、無非同步記憶體）。
3. **功能對拍（比 lint 更能抓截位）**：同一份 TB 在 `PRF_W`=6/7/8 下，8 個測試模式的
   `IPC / retired / st_iq / st_rob / occ_avg / wrongpath / arf_bad` **逐項完全相同**。
   TB 的 `mk()` 也改成走巨集，所以 RUOP 佈局換了不用改 TB。
   欄位若被截掉，mode1/2（相依鏈）的 IPC 會立刻走樣 —— 沒有。

### 7.3 狀態位元：解析推導 vs 實測

`PRF_N` = N、`PRF_W` = P、`RUOP_W` = R。

| 模組 | 狀態位元公式 | 隨 PRF 成長的部分 |
|---|---|---|
| `be_dispatch` | `6 + 2*48 = 102` | **無**（與 PRF 無關） |
| `be_iq` | `1296 + 32*R + 33*N` | `e_pend` 32N（wakeup matrix）+ `wait_m` N |
| `be_eu` | `745 + 68*P + N` | `wh_prf` 64P + `r_wb_prf` 4P + `prf_rdy` N |
| `be_rob` | `781 + 64*P` | `rob_d` 64P（每個 ROB entry 一個 phys dst） |

`be_iq` 展開：`e_v`32 + `e_uop`32R + `e_rob`192 + **`e_pend`32N** + `e_age`1024 + `wait_m`N + counter48。

> **重點更正**：wakeup matrix 是 **`IQ_N × PRF_N`，每個 entry 只有一條遮罩**，不是兩條。
> 因為就緒條件是「所有來源都好了」，src1/src2 可以合併成一條「尚未就緒來源」遮罩
> （`ready = (pend & ~eff_ready) == 0`），不需要分開存。
> 所以 PRF 64 是 **2,048 bit**（不是 4,096），PRF 256 是 **8,192 bit**（不是 16,384）。
> `be_iq` 的**總**狀態（4,688 bit @PRF64）裡，wakeup matrix 只佔 44%，
> 另外 `e_age`(1,024) 與 `e_uop`(1,280) 各佔一大塊。

實測（`synth -flatten` + `abc -g AND,OR,XOR,NAND,NOR,XNOR`，格式 `gates / state_bits`）：

| 模組 | PRF 64 (`PRF_W`=6) | PRF 128 (=7) | PRF 256 (=8) |
|---|---|---|---|
| `be_dispatch` | 451 / 102 | 451 / 102 | 451 / 102 |
| `be_iq` | 81,854 / 4,688 | 117,375 / 7,056 | 176,679 / 11,280 |
| `be_eu` | 13,944 / 1,217 | 15,317 / 1,349 | 18,084 / 1,545 |
| `be_rob` | 20,469 / 1,165 | 21,217 / 1,229 | 22,875 / 1,293 |
| **後段合計** | **116,718 / 7,172** | **154,360 / 9,736** | **218,089 / 14,220** |

**12 個實測值與上面的公式逐一完全吻合**（例如 `be_iq` @PRF256 = 1296 + 32×48 + 33×256 = 11,280）。
這就是「寬度真的打通、沒有被靜默截斷」的證據 —— 只看 lint 過不過是看不出來的。

逐位元對照（Agent F 的做法）：
* `be_rob` 每多 1 bit `PRF_W` → **+64 bit**（64 個 ROB entry 各存一個 phys dst）：1165→1229→1293 ✓
* `be_eu`  每多 1 bit `PRF_W` → **+68 bit**（64 槽完成輪 + 4 條 wb），另加 `prf_rdy` 隨 `PRF_N` ✓
* `be_iq`  每多 1 個 `PRF_N` entry → **+33 bit**（32 條 IQ entry 的 pend 欄 + `wait_m`）
  64→128 = 64×33 + RUOP 加寬 32×8 = 2,368 ✓
* `be_dispatch` 完全不變（451 gates / 102 bit ×3）✓

對照組：我用同一套量法量 `lsu_q` 得到 **1,556 / 1,612 / 1,668**，與 Agent F 回報的數字**逐項相同**
—— 兩邊獨立量到同一個答案，量法本身也被交叉驗證過了。

後段狀態成長：7,172 → 9,736（+2,564，**+36%**）→ 14,220（再 +46%，對 PRF 64 是 **+98%**）。
gate 數 116,718 → 154,360（+32%）→ 218,089（+87%）。

> **模型總狀態的推估**：以監督者的 9,869 bit 為基準（我的後段數字與它一致），
> PRF 128 約 **12.5K bit**（工作集 ~132 KB）、PRF 256 約 **17K bit**（~180 KB）。
> 是 **+73%，不是 3 倍**；300 KB 的估計偏高，因為它假設 wakeup matrix 有兩條遮罩。
> （rename 的 freelist/RAT 也會隨 PRF 長，那部分要問 Agent D。）

### 7.4 掃到 PRF 256 值不值得：結構上的上限是 96

不用等 IPC 數字就能先砍掉一半的掃描空間。PRF 的用量有硬上限：

```
活著的 phys reg = 已 commit 的架構映射 (32，RUOP_ARFD 是 5 bit)
                + 尚未 commit 且有 dst 的 uop 數 (<= ROB_N = 64)
                <= 96
```

每條有 dst 的 uop 在 rename 配一個 phys、在 commit 釋放舊的，
所以「未 commit 且有 dst 的 uop」不可能超過 ROB entry 數。
`ROB_W` = 6 把 ROB 鎖死在 64，於是：

* **PRF 96 以上，freelist 在結構上就不可能耗盡** —— rename stall 會直接歸零
* **PRF 128 已經有 33% 餘裕；PRF 256 在 ROB=64 下是完全用不到的死重**
  （多出來的 160 個暫存器永遠躺在 freelist 裡，但 `be_iq` 的 wakeup matrix
  每條 entry 還是得付 256 bit）
* 現況 PRF 64 只有 `64-32 = 32` 個 freelist 名額，卻要餵 64 個 ROB entry
  —— 這正好解釋 rename stall 51%

**建議 sweep 點：64 / 80 / 96 / 128，不要掃 256**（除非同時放寬 `ROB_W`）。
從 PRF 64 -> 128，`be_iq` 狀態 4,688 -> 7,056 bit（+51%）換 rename stall 51% -> 0，
這筆交易很划算；再往上就只剩成本沒有效益。

### 7.5 `ci/gate_count.sh` 的 2× 問題（請監督者修）

`yosys` 會印**兩個** `=== <module> ===` stat 區塊：`synth` 內建一次、我們最後 `stat` 一次。
`gate_count.sh` 沒有過濾，把兩段都加總 → **gates 與 flops 都剛好是實際值的 2 倍**。

實證（`be_dispatch`，狀態可以手算）：解析推導 `rob_tail(6) + 2 個 48-bit counter = 102`，
raw stat 兩段各 `$_SDFFE_PP0P_ 102`，`gate_count.sh` 報 204。

一行修法：把 `yosys ... | grep` 之間加一段只取最後一個區塊，例如
`| tail -60 | awk '/^=== /{f=1} f'`。

---

## 8. ROB sweep（`ROB_N` = 64 / 128）與那個「容量不足」的 bug

### 8.1 症狀與定位

同一個 binary（`ROB_N`=128 `PRF_N`=192），只改 runtime mask：

| mask | IPC（修前） | ROB stall | 平均佔用 |
|---|---|---|---|
| 64 | 1.391 | 40,445 | 34.14 / 64 |
| 96 | **0.072** | 455,420 | 88.36 / 96 |
| 128 | **0.072** | 455,431 | 118.43 / 128 |

「佔用貼著上限 + retire 幾乎停擺」= commit 沒發生 = **ROB head 的那條 uop 永遠沒 writeback**。

我在 `be_eu` 的兩條掉件路徑上各加一個臨時計數器（debug 複本，不在交付物裡）跑同一條 trace：

```
mask= 64 → drop_wheel 0   drop_mq 0
mask= 96 → drop_wheel 4   drop_mq 0     ← 元兇
mask=128 → drop_wheel 5   drop_mq 0
```

**500,000 拍裡只要掉 4 件，IPC 就從 1.391 崩到 0.072** —— 因為每掉一件就是一個
永久卡住的 head，只有等 wrong-path flush 把整個 ROB 清掉才會恢復。

### 8.2 根因

v3 的 completion wheel 是 4 lane × 16 槽、**容量固定 64**，而且
`lsu_done` 會搶走該 lane 的 writeback port，輸掉的項目要重新排程到後面的空槽
—— 殘留時間因此會超過 uop 自己的延遲。ROB mask 開大之後 in-flight 變多、
`lsu_done` 競爭變密，lane 的 16 槽塞滿，`find_free` 找不到空槽就**靜默丟棄**。

這是**容量不足**不是寬度不符：

* `lint` 抓不到（寬度全對）
* `yosys` 抓不到（合成完全正常）
* **mask 只開到 64 時 100% 正常**，所有既有測試全過

### 8.3 修法：結構上不可能溢位

改成 **robidx 定址的 in-flight pool**（§3.1）。核心不變式：

> ROB entry 在 commit 前不會被重新配置，每條 uop 只會被 issue 一次
> → 每條 in-flight uop 都有一個**專屬**的 slot，且 in-flight 數 ≤ `ROB_N`
> → 不需要配置邏輯、不需要空槽搜尋、**沒有任何路徑會丟件**。

`rq_pend[robidx]` 同理取代了 16 深的 mem request FIFO。

修後（同一條 CoreMark trace，500k 拍）：

| mask | IPC | ROB stall | IQ stall | LSQ stall | 平均佔用 |
|---|---|---|---|---|---|
| 64 | **1.439** | 5,769 | 91,079 | 131,302 | 28.71 |
| 96 | **1.443** | 982 | 91,767 | 133,911 | 29.27 |
| 128 | **1.443** | 12 | 91,917 | 134,580 | 29.16 |

IPC 隨 mask 單調不減 ✓，ROB stall 單調下降 ✓。
**注意 mask=64 也從 1.391 變成 1.439（+3.5%）** —— 舊設計就算沒掉件，
那條「重新排程」路徑本來就在偷偷吃效能。

### 8.4 回歸測試（`backend/run_regress.sh`）

```
backend/run_regress.sh [tree]     # tree 預設 = model/ooo
```

`backend/tb_be_regress.sv` 把四個後段模組接起來（配假 rename / 假 LSU），
在 **`cfg_rob_entries` = W(最小) / `ROB_N`/2 / `ROB_N`(MAX)** 三點 × 10 種 workload 上驗證：

1. 每個 mask 都要有 commit（`retired > 0`）→ 抓「head 永久卡住」
2. **`retired` 不得因 mask 變大而劣化**（容 5% 雜訊）→ 抓容量不足造成的掉件
3. `cmt_arf` 逐條比對（`arf_bad == 0`）

其中 **mode 9 是專門為這個 bug 設計的**：獨立長延遲 ALU + 大量 load，
同時填滿 EU 並製造 `lsu_done` 競爭。實測它確實抓得到：

```
舊 be_eu @ ROB_N=128:  mode=9  mask=4:108  mask=64:896  mask=128:64   → FAIL（變大反而劣化）
新 be_eu @ ROB_N=128:  mode=9  mask=4:108  mask=64:1720 mask=128:1928 → PASS
```

一般的 workload（mode 0~8）**抓不到**這個 bug：mode 7（獨立 lat=15）雖然能把
in-flight 堆到 64，但沒有 `lsu_done` 競爭就不會觸發重新排程。
**「填滿容量」和「填滿容量且有競爭」是兩件事**，回歸測試要涵蓋後者。

### 8.5 成本（`gates / state_bits`，量法同 §7.3）

`be_eu` v4 的狀態公式：

```
be_eu = `ROB_N*(4 + `LAT_W + `PRF_W)   ← pool：v + wt + dv + rq_pend + cnt + prf
      + 2*`ROB_W                        ← 兩個旋轉優先權基準
      + `PRF_N                           ← prf_ready scoreboard
      + 2*`W + `W*`PRF_W + 2*`W*`ROB_W   ← wb / lsu_req 輸出暫存
```

@ROB 64 / PRF 64：`64*14 + 12 + 64 + 8 + 24 + 48 = 1,052` ✓ 與實測逐位相同。

| 模組 | v3（wheel） | v4（pool） | 變化 |
|---|---|---|---|
| `be_eu` | 13,944 / 1,217 | **26,296 / 1,052** | gates +89%，**狀態 −14%** |
| 後段合計 | 116,718 / 7,172 | **129,070 / 7,007** | gates +10.6%，**狀態 −165 bit** |

兩種 `ROB_N` 建置的完整數字（`gates / state_bits`）：

| 模組 | ROB 64 / PRF 64 | ROB 128 / PRF 192 | 狀態公式 |
|---|---|---|---|
| `be_dispatch` | 451 / 102 | 488 / 103 | `` `ROB_W + 2*48 `` |
| `be_iq` | 81,854 / 4,688 | 147,626 / 9,200 | `` IQ_N*(1+RUOP_W+ROB_W+PRF_N) + IQ_N² + PRF_N + 48 `` |
| `be_eu` | 26,296 / 1,052 | 58,904 / 2,350 | 見上方公式 |
| `be_rob` | 20,469 / 1,165 | 44,888 / 2,383 | `` ROB_N*(4+5+PRF_W) + 2*ROB_W+1 + 4*48 `` |

**8 個實測狀態值與公式逐一吻合**（例如 `be_rob` @ROB128/PRF192
= `128*(4+5+8) + 15 + 192 = 2,383`）—— 這是「ROB 寬度真的打通、
沒有被靜默截斷」的證據。

換句話說：**修掉這個 bug 不但沒有讓 bit-sliced 工作集變大，還小了一點**
（pool 拿掉了 wheel 的 `rob` 欄位、`mem_dv` 表與 mem FIFO），
代價是 gate 數 +10.5%（多出來的是 `ROB_N` 寬的優先權挑選與讀出 mux）。

### 8.6 教訓

runtime mask 會給一種假的安全感：**結構建到 128、mask 只開 64，
所有測試都會過，而 64 以上的路徑從來沒被走過。**
這次之後我的回歸測試一律掃 `cfg_*` 的 最小 / 中間 / MAX 三點。
