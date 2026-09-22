# Agent D — 前段模型（`fe_front` + `rn_rename`）

| 檔案 | 模組 | 角色 |
|---|---|---|
| `frontend/fe_front.v` | `fe_front` | fetch + decode queue + redirect |
| `rename/rn_rename.v`  | `rn_rename` | RAT + committed-RAT + freelist，4-wide rename |

埠列由監督者凍結，本次只填 body。語言 Verilog-2005，遵守 `CONTRACT.md §1` 五條硬規則。
介面版本：**ifc.vh v3 + PRF/ROB 參數化 + v8 partial accept**（`RUOP_W` 40 bit，新增 `RUOP_ARFD/ARFDV`；commit 送新 mapping
`cmt_prf`；ready 為 all-or-nothing）。

---

## 1. 驗收結果

`ci/check_contract.sh model/ooo/frontend/fe_front.v model/ooo/rename/rn_rename.v`
→ **PASS=10 FAIL=0**

| top | cells | memories | latch | state bits |
|---|---|---|---|---|
| `fe_front`  | 525  | **0** | 無 | 762（dq 512 + 控制 10 + counter 240）|
| `rn_rename` | 4834 | **0** | 無 | 704 宣告 / 690 實際（x0 的 RAT entry 與 phys0 的 freelist 位元是常數，被折掉）|

`RUOP` v3（32 → 40 bit）對 cell 數**零影響**（4761 → 4761，只多 32 條 wire bit）：
新欄位是 `DUOP_D/DUOP_DV` 的純接線複製，沒有新增任何邏輯。

`stat` 中沒有 `$mem`、`$__RAMGEM_ASYNC_`、`$dlatch`；processes = 0；
`check -assert` 0 problems；`verilator --lint-only -Wall` 0 warning / 0 error。

### cRAT 的代價（監督者要的數字）

| | v1（identity reset） | v2（cRAT） | 差 |
|---|---|---|---|
| cells | 3410 | 4761 | **+1351（+40%）** |
| `$eq` | 1023 | 1405 | +382 |
| `$mux` | 1359 | 1901 | +542 |
| `$and` | 889 | 1276 | +387 |
| flops | 304 | 560 | +256 |

（v3 的 40-bit RUOP 沒有再增加任何 cell：4761 → 4761。）

增量的去處：cRAT 192 flop + cfl 64 flop；4 個 commit lane 的 cRAT 讀 mux 鏈
（4×31 級）；cRAT 的 32 個 entry 各一組 4-lane 寫比較器；commit 新 mapping 的
4 個 64-bit one-hot decoder（`cuse_mask`）；flush 時 `RAT ← cRAT` 的 192-bit mux。
**這些都是結構上必要的**（每個 commit lane 至少要一次 cRAT 讀 + 一次 decode）。
若之後要再瘦身，最大的兩塊是：(a) 12 個 64-bit `fl_oh` decoder（配置 4 + 釋放 4 +
committed 佔用 4），(b) 8 條 32 級的 RAT 讀 mux 鏈。前者可以用 `cfg_prf_entries`
之類的 runtime mask 把 decoder 砍到實際用到的 PRF 範圍，但那需要監督者加 `cfg_*` 埠。
註：`$eq` 的另一邊都是常數，techmap/abc 之後會塌成一棵小 AND 樹，實際 gate 數遠小於
cell 數暗示的量。

### 功能 smoke test

10 組 case 以 `verilator --binary` 跑過，**全數 PASS**：bundle 內 RAW/WAW 旁路、
x0 處理、RAT 跨拍保持、2 拍前端泡泡、block 邊界不跨取、誤預測切 shadow、
flush 回正確路徑、backpressure、freelist 耗盡 + commit 回收、
**flush 從 cRAT 復原（拿到 p35 而非 identity 的 p7）**、
**同一拍 commit 同一個 arch 的連鎖（cRAT[x9] 最後是 p41，p9 與 p40 都回收）**、
**v3 架構目的暫存器欄位（`RUOP_ARFD/ARFDV` 與 `DUOP_D/DUOP_DV` 一致）**、
`cfg_fetch_width` mask、輸出無 X。

testbench **刻意不放進 `frontend/`**（含 `$display`/`$finish`，會被規則 5 的自動掃描
判 FAIL）。目前在 `/tmp/bstmd/tb.v`，監督者若要納入請移到 Agent G 的 `test/`：

```
verilator --binary -Wno-DECLFILENAME -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
  -Wno-UNUSEDSIGNAL -Wno-INITIALDLY -I. --top-module tb \
  --Mdir /tmp/bstmd/obj -o /tmp/bstmd/sim \
  /tmp/bstmd/tb.v frontend/fe_front.v rename/rn_rename.v && /tmp/bstmd/sim
```

---

## 1b. PRF sweep 參數化（64 / 128 / 192 / 256）

兩個模組的實體暫存器寬度與數量**全部走 `` `PRF_W `` / `` `PRF_N ``**，沒有任何硬編的
6 或 64。RUOP 也改成**逐欄位填**（`ru[`RUOP_S1] = ...`）而不是位元連接，所以佈局產生器
把 `RUOP_W` 從 40 改成 48、欄位整體位移、或留保留位元，本模組都不用改。

scratch 路徑：`/tmp/agentd_prf/`（監督者分配）。

| config | lint fe / rn | fe cells / flops | rn cells / flops | rn flop 預測 | 功能測試 |
|---|---|---|---|---|---|
| PRF_N=64  `PRF_W`=6 (RUOP_W 40) | 0 / 0 | 5151 / 666 | 13018 / **594** | 594 | 13/13 PASS |
| PRF_N=128 `PRF_W`=7 (RUOP_W 48) | 0 / 0 | 5151 / 666 | 20215 / **784** | 784 | 13/13 PASS |
| PRF_N=192 `PRF_W`=8 (RUOP_W 48) | 0 / 0 | 5151 / 666 | 27906 / **974** | 974 | 13/13 PASS |
| PRF_N=256 `PRF_W`=8 (RUOP_W 48) | 0 / 0 | 5151 / 666 | 34316 / **1102** | 1102 | 13/13 PASS |

cells/flops 是 `proc; opt; check -assert; techmap; opt` 之後的 gate 數與 flop 數
（techmap 後才數得到 flop 位元數）。四個 config 都是 **latch = 0、memories = 0**。
`lint` 是 `verilator --lint-only -Wall` 的 warning+error 數。

### flop 數的解析推導（不是只看 lint 過）

```
rn_rename flop = RAT(ARF_N x PRF_W) + cRAT(ARF_N x PRF_W) + fl(PRF_N) + cfl(PRF_N)
                 + counter(48 x 2：cnt_st_rename 與 cnt_st_backpressure)
               = 64*PRF_W + 2*PRF_N + 96
   再扣掉常數折掉的部分：x0 的 RAT/cRAT entry（2 x PRF_W）與 phys0 的 fl/cfl 位元（2）
               = 62*PRF_W + 2*PRF_N + 94
```

四個 config 的實測 flop 數與這條式子**逐一相符**（594 / 784 / 974 / 1102），
所以寬度是真的打通到每一級，不是被靜默截斷。
`fe_front` 完全不碰 PRF，四個 config 都是 **666 flop / 5155 gates 不變**
（666 = decode queue 16x32 + 控制 10（含 refill_q）+ 三個 48-bit counter）
—— 這也是它應該有的行為。

每 +1 `PRF_W` → +62 flop（兩份 RAT）；每 +64 `PRF_N` → +128 flop（fl + cfl）。

### 順手修掉的一個 sweep 殺手

`fl_init` 原本是 `{{(`PRF_N/2){1'b1}}, {(`PRF_N/2){1'b0}}}`（一半可配、一半佔用）。
這在 PRF_N=64 時碰巧正確（32 個架構 mapping），但 **PRF sweep 到 128/256 時會平白
少掉一半可配的實體暫存器** —— 模型不會壞、不會 lint 出錯，只會安靜地跑出「PRF 加大
效果打折」的假結論。已改成依架構暫存器數：

```verilog
wire [`PRF_N-1:0] fl_init = {{(`PRF_N-ARF_N){1'b1}}, {ARF_N{1'b0}}};
```

新增的 T11 就是在守這件事：flush 後連續配置直到 stall，累計配出的實體暫存器數
必須**恰好等於 `PRF_N - 32`**（64→32、128→96、192→160、256→224，四個 config 都實測通過）。

### 「碰巧是 6/5/4 但語意不同」的常數已具名

避免下次有人用 regex 把 PRF 寬度一把掃掉：

| 常數 | 值 | 語意 | sweep 時 |
|---|---|---|---|
| `ARFI_W` | 5 | 架構暫存器索引寬度（= `DUOP_D` 寬、ARF_N=32）| **不可**跟著 `PRF_W` 改 |
| `CNT_B` | 6 | 48-bit counter 切成 6 段 8-bit 進位鏈 | **不可**跟著 `PRF_W` / `ROB_W` 改 |
| `` `UC_W `` / `` `LAT_W `` | 4 | uop class / exec latency 欄位寬 | 走巨集 |
| `ARF_N` | 32 | 架構暫存器數 | 獨立維度 |

同類掃描（Agent F 抓到的兩種坑）在本模組**都沒有命中**：
沒有 `[i*`MACRO +: 6]` 這種「起點走巨集、寬度寫死」的切片；
沒有 `reg [31:0]` 的 RUOP_W 遺留（`fe_front` 用 `DQ_W = `DUOP_W`，
`rn_rename` 用 `DW/RW`）。

### 建議：加 `cfg_prf_entries`，PRF sweep 就能收進同一個 batch

現在 PRF 是**編譯期**維度：四個尺寸要四份 netlist、四個 batch。
若監督者願意在凍結埠列加一個 `input [8:0] cfg_prf_entries`，我這邊只需要
**兩行**（`fl_init` 依 cfg 遮罩、配置時不選超出範圍的位元）就能把它變成
契約規則 2 的 runtime mask，四個尺寸共用同一份 netlist。

代價要講清楚：masking 之後**每個 instance 都要付 MAX（PRF_N=256）的成本**
—— rn_rename 從 12787 gate 變成 34085 gate（2.7x），fe_front 不變。
所以「一個 batch 但每個 instance 貴 2.7x」對上「四個 batch 但各自便宜」，
划不划算取決於 batch 排程器怎麼填。若 sweep 點數會再長（例如同時掃 ROB），
runtime mask 才明顯占上風。

---

## 1c. CONTRACT v7：尺寸推導與 `cfg_*` 全範圍驗證

### 對 ROB 的依賴：零

`fe_front` 與 `rn_rename` **完全沒有引用 `` `ROB_N `` / `` `ROB_W `` / `robidx`**
（只有註解提到 ROB）。commit 介面收的是 `cmt_valid/cmt_dv/cmt_arf/cmt_prf`，
不含 ROB index。所以 ROB 64 → 128 對本目錄沒有影響，四個 PRF config 都在
`ROB_N=128 / ROB_W=7` 下重跑過。

### 佔用計數器：本模組也有一個，已改成推導

`be_rob` 的 `rob_cnt` 那一類錯誤（計數器裝不下 MAX 本身）在 `fe_front` 也有對應物：
decode queue 的佔用計數 `dq_cnt_q`。它原本寫死 `[4:0]` / `5'd16`，
**對 DQ_N=16 完全正確**，但改深度就會安靜地溢位。已全部改成推導：

```verilog
localparam DQ_CW = $clog2(DQ_N) + 1;      // 16 -> 5 bit（要裝得下 16 本身，不是 4 bit）
localparam [DQ_CW-1:0] DQ_NC = DQ_N;
localparam TCW   = $clog2(`W) + 1;        // take/present 計數寬度
```

同時把 `de_valid` 的 thermometer 從寫死的 `4'b0000 / 4'b0001 / ...`
改成從 `` `W `` 推導的 generate（寫死的 4-bit 常數在 `` `W `` 變大時會被**靜默零擴展**）。

### 手動展開的 lane 邏輯：加了編譯期斷言

兩個模組的 lane 邏輯（`n_avail` / `n_blk` / 填入 mux / bundle 內旁路鏈）是對
`` `W ``=4 手寫展開的。現在加了 elaboration 斷言，`` `W `` 一變就**編譯失敗**，
不會安靜算錯：

```verilog
generate
if (`W != 4) begin : g_assert_W
    ERROR_fe_front_requires_W_eq_4 bad_W();   // 找不到這個模組 -> hierarchy -check 報錯
end
endgenerate
```

### `cfg_fetch_width` 全範圍驗證（§10 新規則）

本目錄只有一個 `cfg_*`：`cfg_fetch_width`（1..4）。T15 在
**最小值 1 / 中間值 2、3 / MAX 4** 四個點各量 20 拍的交付 uop 數：

| `cfg_fetch_width` | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| 20 拍交付的 uop 數 | 20 | 40 | 60 | 80 |

斷言：非遞減（`thr[4] >= thr[3] >= thr[2] >= thr[1]`）、最小值要能前進、
MAX 要真的比最小值快。前段沒有 retire，用「送進 rename 的 uop 數」當 IPC 的代理。
四個 PRF config 下都是同樣的 20/40/60/80。

### PRF 的「容量」驗證（不只是寬度）

T11 是這條規則要的形式 —— 它驗的是**能真的配出多少個實體暫存器**，
而不是欄位有幾個 bit：

| PRF_N | 64 | 128 | 192 | 256 |
|---|---|---|---|---|
| flush 後可配出的實體暫存器數 | 32 | 96 | 160 | 224 |
| 期望（`PRF_N - 32`）| 32 | 96 | 160 | 224 |

如果哪一級把 `PRF_W` 靜默截斷，或 `fl_init` 回到 `PRF_N/2`，這個數字會立刻不對
（而 lint 與 yosys 都不會有任何聲音）。

### 最終數字（ROB_N=128 / ROB_W=7）

| config | fe gates / flops | rn gates / flops | rn flop 預測 `62W+2N+94` |
|---|---|---|---|
| PRF_N=64 W=6 | 5151 / 666 | 13018 / 594 | 594 MATCH |
| PRF_N=256 W=8 | 5151 / 666 | 34316 / 1102 | 1102 MATCH |

`fe_front` 的 666 = decode queue 16x32 + 控制 10（`dq_cnt_q` 5 + bub 2 + blk_seen 1
+ shadow 1 + refill 1）+ 三個 48-bit counter，**與 PRF/ROB 尺寸無關**，
兩個 config 逐位相同。

---

## 1d. CONTRACT v8：partial accept

```verilog
// fe_front
input  wire [2:0] de_nready;     // 下游這拍能收前 N 條
// rn_rename
output wire [2:0] de_nready;     // 我這拍能收前 N 條
input  wire [2:0] rn_nready;     // 下游這拍能收前 N 條
```

### 為什麼 bundle 內旁路**不需要改**：接受是前綴

這是整個改動的關鍵性質。規則 1 規定「接受的一定是最前面的 n 條」，所以：

```
lane j 被接受  =>  所有 i < j 也被接受
```

而 bundle 內旁路只會「lane j 旁路自 lane i < j」。因此**對每個被接受的 lane，
它的旁路來源全部也被接受** —— 原本整組一起送時算出來的 `ps1_j / ps2_j / p_j`
對前綴而言逐位不變。要改的只有「提交」那一層：

```verilog
wire w0 = acc[0] & nd0;    // acc[j] = (j < n_xfer)
...                        // 只有真的送出去的 lane 才配暫存器、才寫 RAT
```

配額序號 `sl_j`（前面有幾條要配）也一樣：它只數 lane i < j，而那些都被接受了。

### 監督者問的兩個問題

**Q1：保留下來的 uop 的架構映射是重新查 RAT 還是沿用上一拍？**
→ **重新查，而且必須重新查。**
`rn_rename` 是純組合查表（RN 沒有自己的 pipeline register，見 §3.5），
沒被接受的 uop 根本沒離開 `fe_front` 的 decode queue。下一拍 queue 壓縮移位，
它們出現在 lane 0.. 重新查 `rat_q` —— 而 `rat_q` 此時已經含有上一拍被接受的
lane 的寫入。舉例：

```
拍 T   lanes [A B C D]，n_xfer=2      -> A,B 被接受，A,B 的 mapping 在時鐘邊緣寫進 RAT
拍 T+1 lanes [C D E F]（queue 移 2）  -> C 讀 RAT（已含 A,B 的寫入）✓
                                        D 旁路自 C（同拍 lane 0）✓ 程式順序正確
```
「沿用上一拍算好的值」在 C 相依於 A/B 時**也會得到同一個值**（RAT 裡就是那個值），
但在 C 相依於 D 之後的東西時會錯 —— 所以統一重新查，不做任何跨拍快取。

**Q2：上一拍有沒有替沒送出的 lane 預先配實體暫存器？**
→ **沒有。`freelist` 位元只在 lane 真的送出時才清。**
`p_j` 是組合算出來的候選值，但 `alloc_mask` 只包含 `w_j = acc[j] & nd_j`。
沒被接受的 lane 下一拍會重新走一次 pick（拿到的號碼可能相同也可能不同，
反正下游沒看過它），**不會洩漏也不會重複配**。
T17 就是守這件事的指紋測試：讓 `nready` 在 1~3 之間變動地跑到 freelist 耗盡，
可配出的總數必須仍然是 `PRF_N - 32`（64 → 32，256 → 224）。
若沒送出的 lane 也清了 freelist 位元，這個數字會變小。

### 規則 2（`nready` 不得依賴 `valid`）

```verilog
wire [TCW-1:0] n_free = ffd[0]+ffd[1]+ffd[2]+ffd[3];   // 只看 freelist 自己
assign de_nready = (n_free < rn_nready) ? n_free : rn_nready;
```

`de_nready` 只由 **freelist 狀態**與 **下游的 `rn_nready`** 決定，完全不看 `de_valid`
或 `de_duop`。`rn_valid` 也不看 `rn_nready`（`rn_valid = thermometer(min(want, n_free))`），
所以 valid 與 nready 之間沒有任何方向的組合相依。
四種建置的 `verilator --lint-only -Wall` 都 **0 warning、UNOPTFLAT = 0**。

**已知的保守性**：`de_nready` 必須假設「每條 uop 都要配一個實體暫存器」，
因為它不准看 `de_duop`。所以當進來的是 store/branch（不寫暫存器）時，
rename 會低報自己的容量。這是規則 2 的必然代價，不是 bug；
要消除它就得讓 `nready` 看 `valid`，那會立刻形成組合迴圈。

### 三方對 `n_xfer` 的一致性（跨模組相依，請監督者留意）

```
fe_front : 出隊 = min(n_pres, de_nready)
rn_rename: 配置/寫 RAT 的 lane 數 n_xfer = min(want, n_free, rn_nready)
be_dispatch: 取用 = min(popcount(rn_valid), rn_nready)
```
因為 `rn_valid = thermometer(min(want, n_free))` 且 `de_nready = min(n_free, rn_nready)`，
三個式子恆等。**如果 `be_dispatch` 取的比 `min(popcount(rn_valid), rn_nready)` 少**
（例如自己再丟掉 wrong-path uop），rename 就會替沒被收下的 uop 配了暫存器 →
下一拍那條 uop 重新進來再配一次 → **實體暫存器洩漏**。
規則 4 必須被下游嚴格遵守，這條相依建議寫進契約的跨模組相依清單。

### stall 歸因（§8 v2）

| 埠 | 定義 | 單位 |
|---|---|---|
| `cnt_st_fetch` | `de_nready != 0 & n_pres == 0 & ~refill_q & ~flush` | 拍 |
| `cnt_st_refill` | 同上但 `refill_q` | 拍 |
| `cnt_lost_fetch` | `de_nready - n_out`（下游開的名額沒填滿），`~refill_q` | uop-slot |
| `cnt_lost_refill` | 同上但 `refill_q` | uop-slot |
| `cnt_st_rename` | `want>0 & n_xfer==0 & n_free==0 & ~flush` | 拍 |
| `cnt_st_backpressure` | `want>0 & n_xfer==0 & n_free>0 & ~flush` | 拍 |
| `cnt_lost_rename` | `want - min(want, n_free)` | uop-slot |
| `cnt_lost_backpressure` | `min(want, n_free) - n_xfer` | uop-slot |

守恆律（可用來驗證歸因沒有重複或漏算）：

```
cnt_lost_rename + cnt_lost_backpressure == Σ(want - n_xfer)      每拍成立
cnt_st_rename   + cnt_st_backpressure   == 完全停擺的拍數          互斥
cnt_st_fetch    + cnt_st_refill         == 前端完全交不出東西的拍數  互斥
```

歸因順序仍是「先算自己的資源、再算下游」（監督者已裁決；若重疊很大要加第三個
bucket 而不是改優先序）。

### 指紋數字（會隨條件改變，抓得到「跑到舊 binary」）

| 測試 | PRF_N=64 | PRF_N=256 | 抓什麼 |
|---|---|---|---|
| T11 flush 後可配總數 | 32 | 224 | PRF 容量（不只寬度）|
| T17 `nready` 1~3 變動下可配總數 | 32 | 224 | **partial 下的實體暫存器洩漏** |
| T15 `cfg_fetch_width` 1/2/3/4 交付數 | 20/40/60/80 | 20/40/60/80 | cfg 下限回到 1 可用 |
| T16 `nready`=4/3/2/1 的 dst | p7 p9 p32 p33（四者相同）| — | **partial 下的 bundle 旁路** |
| T12 `lost_backpressure` | 精確 +20（4 條 x 5 拍）| — | lost 計數的精確性 |

### 四種建置（`ROB_N` 64/128 x `PRF_N` 64/256）

| 建置 | lint fe/rn | UNOPTFLAT | 功能 | fe gates/flops | rn gates/flops |
|---|---|---|---|---|---|
| ROB 64 / PRF 64 | 0 / 0 | 0 | 17/17 PASS | 5593 / 762 | 13538 / 690 |
| ROB 128 / PRF 64 | 0 / 0 | 0 | 17/17 PASS | 5593 / 762 | 13538 / 690 |
| ROB 64 / PRF 256 | 0 / 0 | 0 | 17/17 PASS | 5593 / 762 | 34836 / 1198 |
| ROB 128 / PRF 256 | 0 / 0 | 0 | 17/17 PASS | 5593 / 762 | 34836 / 1198 |

flop 公式更新為 `rn = 62*PRF_W + 2*PRF_N + 190`（四個 48-bit counter），
`fe = 762`（固定；五個 48-bit counter + decode queue 512 + 控制 10），
四種建置實測逐一吻合。ROB 尺寸對本目錄**完全沒有影響**（兩個模組不引用 `` `ROB_* ``）。

---

## 2. `fe_front` 設計

### 2.1 資料路徑

```
fetch buffer (runtime)          decode queue (深度 16)            rename
 fb_valid/fb_duop  --[n_in]-->  slot0 slot1 ... slot15  --[n_out]-->  de_valid/de_duop
 fb_fe_event                    head 永遠在 slot 0
```

decode queue 是**壓縮式移位暫存器**，不是 RAM：head 固定在 slot 0，出隊 `n_out` 條時
整體往下移 `n_out` 格。每個 slot 的 next-state 只是「5 選 1 的移位 mux + 4 選 1 的填入
mux」，索引全是 genvar 常數，因此 Yosys 不會推斷出任何記憶體（從根本消除
`$__RAMGEM_ASYNC_` 風險）。代價是 512 個 flop 與一組 mux 鏈，但全模組只有 449 cells。

### 2.2 fetch block 與前端泡泡

- `fb_fe_event` 是 **per fetch block**，所以前端**一拍不跨 block 取**：取到第一條
  `DUOP_BLKEND` 就停。否則同一拍會有兩個 block 的 bubble/redirect 事件要處理。
- `FE_BUBBLES(fb_fe_event)` = 0..3：新 block 到達 fetch buffer 頭時載入，之後每拍遞減、
  期間 `fb_take = 0`，歸零後才開始取這個 block 的 uop。bubble 數 N = 恰好 N 拍不供應
  （實測 `FE_BUBBLES=2` → 2 拍 take=0，第 3 拍才取）。
- `n_in = min(連續有效數, block 邊界, cfg_fetch_width, queue 剩餘空間)`，
  全是 3/5-bit 比較（規則 1、2）。

### 2.3 redirect

| 情況 | `fb_redirect` | `fb_redir_shadow` |
|---|---|---|
| 取到 block 最後一條 且 `FE_REDIRECT` 且 `~FE_DIR_OK` 且目前不在 shadow | 1 | 1（切 wrong path）|
| 後端 `flush` / `mispred_in` | 1 | 0（回正確路徑）|
| 兩者同時 | 1 | 0（後端優先）|
| 其他 | 0 | — |

- 單拍脈衝，與 `fb_take` 同一拍給出（造成誤預測的那條分支被取走的那一拍）。
- 進入 shadow 後前端**繼續抓 wrong-path uop**（帶 `DUOP_WRONGPATH`，後端照跑、
  commit 時被 `be_rob` flush 掉）—— 這就是誤預測懲罰的來源。
- `shadow_q` 保證同一次誤預測不會重複觸發或重複計數。
- `mispred_in` 與 `flush` 在 `top.v` 接同一條線（監督者已確認語意）：
  `mispred_in` 用來「回正確路徑」，`flush` 用來「清 decode queue」。

### 2.4 counter 語意與 stall 歸因

**歸因原則（監督者裁決）：每個 stall cycle 必須恰好有一個擁有者，而且必須是真正的來源。**
「下游收得下、但前端一條 uop 都交不出來」這個集合被切成**互斥的兩半**：

| 埠 | 條件 | 意義 |
|---|---|---|
| `cnt_st_fetch`  | `starve & ~refill_q` | **真正的前端斷流**：I-cache 泡泡、fetch buffer 沒東西 |
| `cnt_st_refill` | `starve &  refill_q` | **flush 後的重填拍**：真正的來源是誤預測，不是 fetch 頻寬 |
| `cnt_mispred`   | 切 shadow 的次數 | 前端偵測到的方向誤預測（事件數，不是拍數）|

其中 `starve = de_ready & (de_valid == 0) & ~flush`。

- **兩者相加 == 舊版的 `cnt_st_fetch`**：一拍都沒有多算、也沒有漏掉。
  舊版把重填拍混在 `cnt_st_fetch` 裡，上界是 `cnt_mispred x 重填深度`，
  以 CoreMark 的誤預測次數看可能到幾萬拍，足以讓「前端斷流」的結論整個偏掉。
- `refill_q`：`flush` 時置 1，**第一次成功送出 uop**（`de_ready & de_valid != 0`）時清 0。
- **`rst` 不算 refill**：冷啟動填管線記在 `cnt_st_fetch`。`cnt_st_refill` 的定義是
  「flush 之後」，reset 不是 flush。
- **flush 當拍兩邊都不計**：那是誤預測懲罰本身，事件已記在 `cnt_mispred`。
- 下游回壓的拍（`de_ready = 0`）兩邊都不計 —— 那時 `starve = 0`，
  由下游自己的 counter 認領，不會重複。
- `cnt_st_fetch` **不含**「交得出但不足 4 條」的部分斷流（那不是全停，無法獨立歸因）。

驗證（T14）：flush 之後連續 4 拍沒有 uop 可送 → `cnt_st_refill` **精確 +4**、
`cnt_st_fetch` 完全不動；送出一條 uop 離開重填視窗後再餓 3 拍 →
`cnt_st_fetch` **精確 +3**、`cnt_st_refill` 不動。

---

## 3. `rn_rename` 設計

### 3.1 結構

- **推測 RAT**（`rat_q`）：32 entry × 6 bit 攤平向量。讀走 32 級 mux 鏈；
  寫是每個 entry 一組 4-lane 比較器，`uop3 > uop2 > uop1 > uop0`，
  所以**同一拍 bundle 內的 WAW 自動由最後一條贏**。`rat_q[x0]` 永不寫，固定 phys 0。
- **committed RAT**（`crat_q`）：同樣結構，只被 commit 更新，代表架構狀態。
- **推測 freelist**（`fl_q`）：64-bit bitmask，1 = 可配。一拍最多配 4 個，用 4 級
  「取最低可用位元 → 清掉 → 再取」的 priority pick。phys 0 保留給 x0，永不配出。
- **committed freelist**（`cfl_q`）：架構狀態下的 freelist，flush 時直接灌回 `fl_q`。
  維護成增量更新（見 §3.3），而不是每拍從 cRAT 重算 —— 後者要 32 個 64-bit decoder。
- 全檔沒有任何 `case`（規則 4），只有 if/else 與 `?:`。

### 3.2 bundle 內相依（4-wide）

第 j 條 uop 的每個來源，先看 j 之前是否有 uop 寫同一個架構暫存器，有就用那條**最新**
的新 mapping，沒有才讀 RAT。實測（T1）：

```
u0: x1 = x2 + x3   -> d=p32, s1=p2
u1: x4 = x1 + x1   -> s1=s2=p32（旁路 u0）        d=p33
u2: x1 = x4 + x0   -> s1=p33（旁路 u1）, s2 無效   d=p34（WAW over u0）
u3: x5 = x1 + x2   -> s1=p34（拿 u2 的新 mapping，不是 u0 的）
```

x0 規則：`dst == x0` → 不配實體暫存器、`RUOP_DV = 0`；
`src == x0` → `RUOP_S1V/S2V = 0`（x0 永遠 ready，IQ 不該等它 wakeup）。

**架構目的暫存器（介面 v3）**：`RUOP_ARFD/ARFDV` 是 `DUOP_D/DUOP_DV` 的**原樣複製**
（未經 x0 gating），供 `be_rob` 產生 `cmt_arf`；實體暫存器仍走 `RUOP_D/RUOP_DV`。
因此 `dst == x0` 的 uop 會出現 **`ARFDV = 1` 但 `RUOP_DV = 0`**（架構上有寫 x0、
實體上沒配暫存器）。這對 cRAT 是安全的：ROB 會送出 `cmt_arf = 0`，
而 commit 守衛 `(cmt_arf != 0)` 會把它丟掉，不會污染 cRAT 或誤放 phys 0。
**給 Agent E 的提醒**：若 `be_rob` 想用「這條 uop 有沒有配實體暫存器」來決定
`cmt_dv`，該用 `RUOP_DV` 而不是 `RUOP_ARFDV`。

### 3.3 commit 介面 v2 與 cRAT

ROB 送來的是**新 mapping**，舊的由 rename 自己查：

```
old = cRAT[cmt_arf];        // 32 級 mux 鏈，用完整 6-bit arch 比對
free(old)  if old != 0;     // 回 freelist（推測與 committed 兩份都放）
cRAT[cmt_arf] = cmt_prf;    // 新 mapping 進架構狀態
```

- **同一拍 commit 同一個架構暫存器**（連續 WAW 一起退休）有做 bundle 內連鎖：
  後面 lane 看到的 old 是前面 lane 剛寫入的 `cmt_prf`，中間的 mapping 全部回收。
  cRAT 的寫入由最後一個 lane 勝出。（T10 驗證：commit x9=p40 + x9=p41 →
  p9 與 p40 都回收，cRAT[x9]=p41。）
- `cfl_nxt = (cfl_q & ~cuse_mask) | free_mask`：先清掉「架構上開始使用」的新 mapping，
  再放回被釋放的舊 mapping。**釋放優先於佔用**，這樣上面那個連鎖情況下 p40
  才會正確地留在 free 狀態。
- `cmt_arf == 0` 的 lane 一律忽略（x0 不配暫存器）；`cmt_arf >= 32`（demo 不會產生）
  在 cRAT 讀不到任何 entry、回傳 0，被 `old != 0` 守衛擋掉，不會誤放 phys 0。

### 3.4 flush 復原

```
rat_q <= crat_nxt;      // 含「本拍的 commit」
fl_q  <= cfl_nxt;
```

用 `crat_nxt` 而不是 `crat_q` 是刻意的：`be_rob` 是在 **commit 階段**發現 wrong-path uop
才拉 `flush`，同一拍它也會 commit 那條 uop 之前的正確 uop。用 next-state 才能把那些
commit 一起算進復原後的架構狀態。

**精確性條件**：`be_rob` 的 flush 是「commit 走到 wrong-path uop 時拉 flush，並丟掉
ROB 內其餘全部項目」。也就是說 flush 當下**沒有任何比 flush 點更老的 in-flight uop**，
所以 cRAT 就是完整的正確狀態 —— 這個復原是**精確的，不是近似**。
（若 Agent E 之後改成「ROB 中途 flush」（比 flush 點老的 uop 還在 in-flight 未 commit），
則 cRAT 會少掉那些 uop 的 mapping、cfl 會把它們的 phys 誤判為可配 —— 屆時必須改成
checkpoint 或 ROB walk。這個相依關係請監督者保留在契約裡。）

### 3.5 握手（ifc.vh v2：all-or-nothing）

| 訊號 | 語意 |
|---|---|
| `de_ready` | `rn_ready & enough_free`。拉高 = 本拍把 `de_valid` 標示的 uop **全部**收下。`fe_front` 照同一語意出隊。 |
| `rn_valid` | `de_valid & {4{enough_free}}`，**刻意不看 `rn_ready`**，避免 valid→ready→valid 組合迴圈。實際傳遞發生在 `rn_valid & rn_ready`。 |

`rn_rename` 是**組合查表 + 邊緣更新狀態**：RN 階段的 pipeline register 實際上是下游
`be_dispatch` 的輸入暫存器，這讓 4-wide bundle 旁路不必再多一層旁路網路。

### 3.6 counter 語意與 stall 歸因

**歸因原則（監督者裁決，2026-09-21）：一個 stall cycle 只能被歸因到一個來源，
而且必須是真正的來源。** 本模組輸出兩個互斥的 counter：

| 埠 | 條件 | 意義 |
|---|---|---|
| `cnt_st_rename` | `(de_valid != 0) & ~enough_free & ~flush` | **rename 自己的資源不夠**：freelist 耗盡，配不出實體暫存器 |
| `cnt_st_backpressure` | `(de_valid != 0) & enough_free & ~rn_ready & ~flush` | **有能力送但下游不收**：`rn_ready = 0` |

- 兩者由 `enough_free` 互斥，同一拍不會同時 +1。
- **下游是誰造成的不由本模組認定**：`rn_ready` 被 IQ / LSQ / ROB / MSHR 任何一個拉低
  都算 `cnt_st_backpressure`，實際兇手由下游自己的 `cnt_st_iq / lsq / rob / mshr` 指認。
- **兩者都排除 flush 拍**：`be_dispatch` 在 flush 時會拉低 `rn_ready`，那是誤預測懲罰
  （事件已記在 `cnt_mispred`、被丟掉的 uop 記在 `cnt_wrongpath`），不是資源不足。
- 同一拍「freelist 也空、下游也不收」時記在 `cnt_st_rename`（自己的資源優先）。
  此時兩個原因同時成立、單修一邊都救不了。
  **若之後 sweep 顯示這個重疊很大，要加第三個 bucket，不要改優先序**
  （監督者已採納此判斷）—— 改優先序只是把偏差換個方向，加 bucket 才是真的解決。

#### 修正紀錄：舊版會把下游回壓算到 rename 頭上

v1 的條件是 `(|de_valid) & (~rn_ready | ~enough_free) & ~flush`，
`~rn_ready` 那一項把 IQ / LSQ / ROB / MSHR 的回壓全部記成「rename stall」，
與下游自己的 counter **重複計數**，導致 PRF sweep 得出「rename 佔 51% stall、
PRF 64 配 ROB 64 失衡」的錯誤結論（真正的瓶頸是 LSQ 與 IQ）。
現已拆成上表兩個互斥 counter。

驗證（T7 / T12 / T13）：
- freelist 耗盡時只有 `cnt_st_rename` 增加，`cnt_st_backpressure` 維持 0；
- `rn_ready = 0` 且 freelist 有餘時，`cnt_st_backpressure` **精確增加 5**（5 拍），
  `cnt_st_rename` 完全不動；
- flush 拍（同時 `rn_ready = 0`）兩個 counter 都不動。

---

## 4. 已知限制與簡化

1. **架構暫存器只用 x0..x31**（`ARF_W` 6 bit 但 `DUOP_D` 只有 5 bit）。FP 不進 trace，
   沒有獨立的 FP RAT。`DUOP_S1/S2` 的 bit5 只參與相依比對，RAT 索引取低 5 bit；
   cRAT 則用完整 6-bit 比對（arch ≥ 32 不會命中）。監督者已裁定為已知限制。
2. **一拍不跨 fetch block 取 uop**（§2.2）。block 很短時會低估 fetch 頻寬，
   但這是 `fb_fe_event` per-block 語意的必然結果。
3. **decode queue 深度固定 16**，沒有對應的 `cfg_*`（契約 §3 沒給前端 queue 的配置暫存器），
   所以這一維無法做 DSE。監督者已接受為已知限制。
4. **`cnt_st_fetch` 只計「完全交不出 uop」**，不計「交得出但不足 4 條」
   （那不是全停，無法乾淨地歸因給單一來源）。flush 後的重填拍已分離到 `cnt_st_refill`。
5. **rename 沒有自己的 pipeline register**（§3.5），RN 的拍數由 `be_dispatch` 負責。
6. **`cfg_fetch_width = 0` 會讓前端永遠不取**（非法配置），由 runtime 保證 ≥ 1。
7. flush 復原的精確性依賴 `be_rob` 的 commit-time flush（§3.4）。

---

## 5. 監督者裁決後的介面問題狀態

| # | 問題 | 狀態 |
|---|---|---|
| 1 | RUOP 無 old-phys，ROB 無法產生舊 mapping | **已解**：ifc.vh v2 改送 `cmt_prf`（新 mapping），rename 用 cRAT 推導舊的；flush 復原一併改用 cRAT。已實作。 |
| 1b | RUOP 不帶架構暫存器編號，ROB 只能拿 `cmt_prf[4:0]` 當 `cmt_arf` → cRAT 被指到錯的 index（安靜出錯） | **已解**：ifc.vh v3 把 `RUOP_W` 加寬到 40 bit，rename 填 `RUOP_ARFD/ARFDV`。已實作，cell 數不變。 |
| 2 | `fb_mem_event` 與 dispatch 時間錯位 | 已採納，修在 LSU 側（`lsu_q` 自建 side FIFO）。`fe_front` 不需改動。 |
| 3 | `mispred_in` 與 `flush` 同線 | 維持現狀，語意已寫進 ifc.vh。 |
| 4 | `cnt_mispred` 歸屬 | 歸前端（本模組），`be_rob` 不重複計。 |
| 5 | ready 單 bit | 定為契約（all-or-nothing），已寫進 ifc.vh。 |
| 6 | `ARF_W`(6) vs `DUOP_D`(5) | 已知限制，不改（demo 無 FP）。 |
| 7 | decode queue 無 `cfg_*` | 已知限制；`cfg_fetch_width ≥ 1` 由 runtime 保證。 |
| 8 | `cnt_st_rename` 把下游回壓也算進去（重複計數，害 PRF sweep 得出相反結論）| **已解**：拆成互斥的 `cnt_st_rename`（freelist 不夠）與新增的 `cnt_st_backpressure`（下游不收），flush 拍兩者都排除。埠由監督者加進 `top.v`。 |
| 9 | `cnt_st_fetch` 把 flush 後的重填拍算成前端斷流 | **已解**：監督者採方案 (a)，新增 `cnt_st_refill` 埠（`top.v` v6 已接）。兩者互斥且相加等於舊值。 |

§3.4 的相依已由監督者確認並寫進契約：`be_rob` 是 commit-time flush（走到 wrong-path uop
才拉 flush 並丟掉 ROB 其餘全部項目），所以 **cRAT 復原是精確的，不是近似**。
未來若有人把 `be_rob` 改成 ROB 中途 flush，rename 必須同步改成 checkpoint / ROB walk，
否則會把仍在 in-flight 的老 uop 的實體暫存器誤放回 freelist（已有回歸檢查把關）。
