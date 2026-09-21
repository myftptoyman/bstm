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

### 3.1 completion wheel（只有倒數，沒有運算）

每個 issue lane 一組 16 槽的環形表（16 = 2^`LAT_W`），`wh_ptr` 每拍 +1。
issue 一條 lat=L 的 uop 就把它放進 slot `wh_ptr + L - 1`，
等指標轉到那一格時 pop 出來登記 writeback（下一拍 `wb_valid` 拉起）。
**沒有任何 ALU / 資料路徑**，整個 EU 就是指標 + 佔用位元（PLAN §5.4）。

* lat<=1 的 uop 不進輪子，直接在 issue 當拍登記 writeback。
* 目標槽被同 lane 先前的 uop 佔住時，往後找第一個空槽（bounded 的結構冒險模型），
  找不到不會丟掉；16 槽對單 lane 必定有空位。
* 每個 lane 每拍最多完成 1 條 → 剛好對上 4 條 writeback port。
  這等同「4 個 write port 靜態切給 4 個 issue lane」，比真實硬體略嚴格，
  在同 lane 完成撞車時會讓延遲多 1 拍以上。

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

`UC_LOAD` / `UC_STORE` / `UC_AMO` 不在 EU 完成：進一條 16 深的 request queue，
每拍在 `lsu_ready` 時最多送 4 條 `lsu_req_*`（issue 到送出有 1 拍延遲），
回來的 `lsu_done_*` 直接轉成 writeback。
`lsu_done_*` 沒有帶 dv 位元，所以 EU 用一條 64 bit 的 `mem_dv`（以 robidx 索引）
記住每條 mem uop 有沒有 dst，避免 store 回來時亂 set `prf_ready`。

writeback 埠仲裁優先序：`lsu_done` > 輪子 pop > 本拍 lat<=1。
`lsu_done` 是輸入、留不住，所以必須最高；輸掉的另外兩者會被排回輪子（不會遺失）。

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

### 自測（verilator `--binary`，testbench 放在 /tmp，不屬於交付物）

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
4. **EU 每 lane 每拍最多完成 1 條**（見 3.1），同 lane 撞車會讓延遲變長。
5. **mem request queue 16 深，滿了會丟請求**：`lsu_ready` 長時間拉低才可能發生
   （dispatch 端有 `lsq_full` 擋著，在 LDQ+STQ=32 的配置下極難觸發）。
   一旦丟掉，那條 uop 的 ROB entry 永遠不會 done → 卡死。
   要根治同樣需要 EU -> IQ 的 backpressure 埠（目前沒有）。
6. **flush 後 `prf_ready` 全設 1**。管線整個清空，沒有 in-flight 的 producer，
   這是最簡單且不會卡死的選擇；rename 端重建 RAT 後配出來的 phys reg
   會由 IQ 的 `wait_m` 重新擋住，語意仍然正確。
7. ~~`be_eu` 的 `$shift`/`$shiftx`~~ —— v3 已全部改成常數索引展開，四個模組皆為 0。
8. `be_rob` 的 `rob_full` 用本拍開始時的佔用判斷，ROB 剛好卡在滿的邊緣時會多 stall 1 拍。
