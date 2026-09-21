# `model/ooo/lsu/` — LDQ / STQ / MSHR（Agent F）

檔案：`lsu_q.v`（唯一交付，埠列由 `top.v` v2 凍結）

BSTM 把 cache 的 hit/miss 與**無負載延遲**放在離線算好，經 `fb_mem_event`
（`MEM_LEVEL` = bit[1:0]、`MEM_LAT_CLASS` = bit[7:4]，查 `bstf_lat_table`）
餵進模型。本模組負責的是**離線算不出來的那一半**：MSHR 佔用、記憶體埠競爭、
store-load 排序 —— 也就是 PLAN §11 R2 要補回來的 memory-level parallelism。

---

## 1. 結構總覽

| 結構 | MAX | 每 entry 欄位 | mask |
|---|---|---|---|
| LDQ | 16 | `v(1) ar(1) st(1) blk(1) dn(1) rob(6) prf(6) lvl(2) lc(4) wid(4)` = 27 bit | `cfg_ldq_entries` |
| STQ | 16 | `v(1) ar(1) st(1) dn(1) noacc(1) rob(6) prf(6)` = 17 bit | `cfg_stq_entries` |
| MSHR | 8 | `v(1) dead(1) cnt(8) rob(6) prf(6) ldq(4)` = 26 bit | **固定 8**，見 §6 |
| L1-hit 延遲線 | 8 級 × 2 slot | `v(1) q(1) rob(6) prf(6) idx(4)` = 18 bit | — |
| LSU 請求緩衝 | 8 | `rob(6)` | — |
| mem-event side FIFO | 32 | `{lat_class(4), level(2)}` = 6 bit | — |

所有陣列都是 **flat packed `reg`**，索引一律用常數展開的 `for` 迴圈做
one-hot 比較 —— 不會推出 `$mem`、不會有非同步讀、不會有 latch。

---

## 2. 資料流（一拍之內的處理順序）

```
 1  LDQ dealloc（head，<=2/cycle）
 2  STQ dealloc（head，<=2/cycle）        -> 產生 store dealloc index
 3  用 store dealloc index 解除被卡住的 load
 4  延遲線 stage0 完成                     -> done lane 0 / 1
 5  延遲線移位
 6  MSHR countdown-1；歸零者回報           -> done lane 2 / 3（<=2/cycle）
 7  請求緩衝送 2 筆 rob 進 CAM             -> 設 LDQ/STQ 的 address-ready
 8a 記憶體埠仲裁：<=2 個存取 / cycle（load 優先）
 8b 發動：L1 hit -> 延遲線；miss -> 配 MSHR（滿則 stall）
 9  收新的 LSU 請求（<=W 筆進緩衝）
10  dispatch 配置 LDQ/STQ；從 mem-event FIFO pop 事件；fetch 側 push
11  flush
12  counter
```

### mem-event side FIFO（v2 監督者裁決）

`fb_mem_event` 是 **per fetch-buffer slot**，而 LSQ 是在 **dispatch** 才看到
uop，中間差了數拍，直接用 lane index 對應會拿到別條指令的 cache 事件。
因此本模組內建一個 side FIFO：

- **push（fetch 側）**：本拍實際被取走的 uop（`fb_valid[i] & (i < fb_take)`）
  中 `DUOP_CLASS ∈ {UC_LOAD, UC_STORE, UC_AMO}` 者，把
  `{MEM_LAT_CLASS, MEM_LEVEL}` 壓成 6 bit 推進 FIFO
- **pop（dispatch 側）**：`ds_valid` 中的記憶體 uop 依序取出
- 記憶體 uop 一定依程式順序 dispatch，FIFO 順序天然正確
- 深度 32（≥ decode queue 16 + rename/dispatch 級數）
- **flush 清空**（wrong-path 的 mem event 丟掉；runtime 需把 `.mem` 游標倒回）
- FIFO 空時 pop 會拿到 `{lc=0, lvl=0}`，即「L1 hit、3 拍」。這是安全的退化值，
  但如果它大量出現代表 push/pop 失步 —— 建議 runtime 對拍時監看

---

## 3. MSHR 與 MLP（本模組的重點）

- load 在發動時看 `MEM_LEVEL`：
  - `== 0`（L1 hit）：**不佔 MSHR**，用固定延遲 `bstf_lat_table[lat_class]`
    走延遲線完成
  - `!= 0`（miss）：**一定要配一個 MSHR**，`countdown = bstf_lat_table[lat_class]`
- 每拍所有 valid MSHR 的 countdown 減 1，歸零時回報完成並釋放
- **MSHR 滿 → 新的 miss stall**：該 LDQ entry 保持 `ar=1, st=0`，下一拍重試，
  同時 `cnt_st_mshr` +1。**這就是 MLP 的硬上限**，也是整個模型裡
  「ROB 能同時藏住幾個 miss」的決定因素
- 同時最多 8 個未完成 miss；配合 2 個 LSU port 與 16-entry LDQ，
  這三者共同決定 memory-bound workload 的行為

### counter 語意

`cnt_st_mshr` = **「本拍有 miss 想要 MSHR 但一個都拿不到」的 cycle 數**。
- 每拍最多 +1（是 cycle 計數，不是 miss 計數）
- 一個 miss 連續卡 N 拍會貢獻 N
- MSHR 有空但被 LSU 埠限制而沒發動，不算在內

---

## 4. 簡化清單（重要，影響準確度）

### 4.1 store-load disambiguation 是**機率模型**，不是真的比位址

demo 的 trace 裡沒有位址（建模規則 1 明文禁止位址進模型），所以無法做真正的
age-ordered 位址比對。本模組的做法：

- 每條 load 在 **dispatch 配置 LDQ entry 時**擲一次決定性的偽隨機：
  `rnd < CONF_THR` 就判定與更早的 store 衝突（`CONF_THR=8`，即 **8/256 ≈ 3.1%**）
- 衝突時記下「當下 STQ 裡最年輕的那個 store」的 index（`wid`，4 bit），
  該 store 從 STQ dealloc（= commit）之後才解除封鎖
- 被封鎖的 load 不參與記憶體埠仲裁，但**照樣佔著 LDQ entry**
  （這正是 disambiguation 影響效能的主要途徑）
- 偽隨機是兩個 8-bit Fibonacci LFSR（poly `x^8+x^6+x^5+x^4+1`）：
  `la` 每拍走 4 步、`lb` 每配一條 load 走 1 步，`rnd = step_i(la) ^ lb`。
  **完全決定性**（reset seed 固定 `la=0xA5, lb=0x3C`），bit-exact 對拍沒問題

**已知偏差**：
- 真實的衝突率是位址與 workload 相依的，這裡是固定機率 → 對 store-heavy
  的迴圈會低估、對 pointer-chasing 會高估
- 「等到該 store commit」比真機保守（真機有 store-to-load forwarding，
  只要 store 的資料備妥就能轉送，不必等 commit）→ **系統性高估懲罰**
- 只跟「最年輕的 store」相依，不是真正的 age-ordered 多重相依
- `CONF_THR` 是 module parameter，之後可以拿 gem5 correlation 來校準

### 4.2 MSHR **不做 merging**

真機會把**落在同一條 cache line** 的多個 miss 併進同一個 MSHR（secondary
miss），只佔一個 entry。demo 沒有位址，無從判斷是否同 line，所以本模組
**每個 miss 各佔一個 MSHR**。

**影響方向很明確：會低估 MLP**。
- 空間局部性高的存取（陣列掃描、struct 連續欄位）在真機上常常好幾筆
  miss 共用一個 MSHR；這裡會把 8 個 MSHR 提早吃光
- 結果是 `cnt_st_mshr` 偏高、memory-bound workload 的 IPC **偏悲觀**
- 偏差量隨 ROB/LDQ 變大而變大（能同時發出的 miss 變多）→ 會影響**趨勢正確性**
- 注意這跟 PLAN §11 R2 講的「7.4 造成偏樂觀」方向相反，兩者會部分互相抵銷，
  但不該指望它們剛好抵銷
- **緩解途徑（離線側）**：Agent B 的 cache sim 看得到位址，可以在
  `MEM_*` byte 裡多放一個 bit 標記「這筆是 secondary miss（可 merge）」，
  模型看到就不配 MSHR 而是掛到既有的。這需要改 `include/bstf.h`，
  是監督者的決定

### 4.3 其他簡化

| 項目 | 做法 | 影響 |
|---|---|---|
| store commit | 埠列沒有 commit 回授，改用「STQ in-order dealloc」當代理：store 完成（寫進 store buffer，固定 `ST_LAT=2`）後從 head 依序排掉，<=2/cycle | store 的釋放比真正 commit 早，disambiguation 解鎖偏早 |
| store 的 cache 事件 | 忽略，一律當 store buffer hit（`ST_LAT=2`），不配 MSHR | 不模型化 write-allocate 造成的 MSHR 壓力 |
| 記憶體埠仲裁順序 | index priority（不是真正的 age order），load 優先於 store | 高負載時的服務順序與真機有出入；不影響吞吐上限 |
| L1-hit 延遲 | 延遲線 8 級，latency 夾在 1..8 | `lat_class` 對應 >8 拍卻標成 L1 hit 的情況會被截斷（實務上不會發生：level 0 的 class 是 0/1/10/11 → 3/4/2/8） |
| 延遲線 slot 衝突 | 每級 2 slot；目標級沒空位時該存取當拍不發動，下拍重試 | 罕見（<=2 存取/拍），會造成 1 拍延後 |
| LSU pipe 深度 | `req` 進緩衝 → 下一拍 CAM 設 ar → 同拍仲裁發動，約 2 拍 AGU/埠延遲 | 固定偏移，不影響趨勢 |
| `flush` | 埠列沒有 `flush_robidx`，只能整組清空 LSQ/延遲線/緩衝。**MSHR 例外**：已送出去的 miss 收不回來，改標 `dead` 繼續倒數、佔著 entry 但不回報 | 與 `be_eu`/`be_dispatch` 的 flush 語意一致（它們也是整組清空）。`dead` 的設計保住了 **wrong-path miss 污染 MSHR** 這個真實效應 |
| AMO | 配一個 LDQ entry（當 load 處理，會佔 MSHR）+ 一個 STQ entry 當排序佔位（`noacc=1`，不佔埠、不回報 done） | CoreMark 沒有 AMO；只保證不會 double-writeback |
| wrong-path uop | 照常佔 LDQ/STQ/MSHR | 這是刻意的，wrong-path 的資源佔用要算進去 |

---

## 5. 介面約定（給 Agent E / 監督者）

- **`lsq_full`**：保守訊號，代表「整組 W=4 條 dispatch 不保證放得下」。
  `be_dispatch` 是 all-or-nothing 收，所以這樣才安全。
  **因此 `cfg_ldq_entries` / `cfg_stq_entries` 必須 >= W(4)**，
  否則會永遠 full（模型會停住）。
- **`lsu_ready`**：`be_eu` 會把 pop 決定暫存一拍，所以 ready 只在請求緩衝
  全空時拉高；緩衝深度 8 足以吸收「ready 慢一拍」造成的兩個連續 burst
  （最壞佔用 6）。**`lsu_ready` 不受 MSHR 滿影響** —— MSHR 的反壓是靠
  LDQ 塞滿 → `lsq_full` → dispatch stall 這條路徑，這才是真正的 MLP 限制機制。
- **`done_v[3:0]`**：lane 0/1 = L1-hit 延遲線，lane 2/3 = MSHR
  （= 2 個 return bus，每拍最多 2 個 miss 回報）。
- **`done_prf`**：直接是該 uop 的 `RUOP_D`。埠列**沒有 `done_dv`**，
  所以 store 這種沒有 dst 的 uop 也會帶回一個沒有意義的 prf 值。
  `be_eu` 目前用自己的 `mem_dv[rob]` 表判斷要不要 set `prf_ready`，這樣是對的。
- **`mshr_full`**：純觀測用（`top.v` 沒有接到別的模組）。
- **`req_ev`**：未用（`be_eu` 也沒有驅動 `lsu_req_ev`），保留。

---

## 5b. 寬度參數化（PRF / ROB sweep）

本模組**沒有任何硬編的 PRF / ROB 寬度**。所有帶實體暫存器或 ROB index 的欄位
都走 `` `PRF_W `` / `` `ROB_W ``：

| 走 `` `ROB_W `` | 走 `` `PRF_W `` |
|---|---|
| `ld_rob` `st_rob` `ms_rob` `hl_rob` `pq_rob` `d_rob` `sr0/1` `g_rob0/1` `c_rob` | `ld_prf` `st_prf` `ms_prf` `hl_prf` `d_prf` `g_prf0/1` `c_prf` |

注意 `ld_rob`/`ld_prf`（及 st/ms/hl 的對應項）**必須分開宣告** —— 它們原本共用
一個 `[N*6-1:0]`，PRF 與 ROB 寬度分家之後合併宣告就錯了。

**不隨 PRF/ROB 變動的 6：** mem-event FIFO 的 payload 是
`MEV_W = 6`（`{lat_class(4), level(2)}`），以及 `me_cnt`（MQN=32 的佔用計數）。
已經具名成 `MEV_W` 並加註解，避免下次 sweep 被誤改。

**LSQ index 欄位**（`ld_wid` `ms_ldq` `hl_idx`，各 4 bit）綁的是
`LDN`/`STN` = 16（CONTRACT §0 的 MAX 表固定），不是 PRF/ROB，不需要跟著掃。

### 驗證結果

`` `PRF_W `` = 6 / 7 / 8（`PRF_N` = 64 / 128 / 256，`RUOP_W` = 40 / 48 / 48，
`RUOP_D` / `RUOP_S1` / `RUOP_S2` 隨 `PRF_W` 加寬）三種都：

| `PRF_W` | verilator `-Wall` | yosys `check -assert` | cells | flop bits |
|---|---|---|---:|---:|
| 6 | 0 warning 0 error | 0 problems | 12,743 | 1,556 |
| 7 | 0 warning 0 error | 0 problems | 12,748 | 1,612 |
| 8 | 0 warning 0 error | 0 problems | 12,748 | 1,668 |

flop 增量 = **每 PRF bit +56**（LDQ 16 + STQ 16 + MSHR 8 + 延遲線 16 = 56 個
`prf` 欄位），跟解析預期完全吻合 —— 代表寬度真的打通了，沒有被靜默截斷。

**成本結論：PRF sweep 對本模組幾乎是免費的**（64→256 只多 112 個 flop、
gate 數在雜訊內）。PRF 大小在 `lsu_q` 已經不是結構性維度，
可以跟別的配置塞進同一個 bit-slice batch。

## 6. `cfg_mshr_entries` 未接線

`top.v` 沒有把 `cfg_mshr_entries` 接到 `lsu_q`。監督者已確認**維持固定
`MSHR_N = 8`**。程式內以 `localparam MSN = 8` 表示。
如果之後要掃 MSHR 數量（這是 MLP 研究最該掃的旋鈕），只需要：
1. `top.v` 加 `.cfg_mshr_entries(cfg_mshr_entries)`
2. 本模組加 `input wire [3:0] cfg_mshr_entries`，
   把 free-slot 掃描的 `~n_ms_v[m]` 換成 `~n_ms_v[m] & (m < ms_max)`，
   `mshr_full` 換成「前 `ms_max` 個都 valid」
成本影響 < 100 cell。

---

## 7. 成本

驗收用的 `yosys proc; opt` cell 數 = **12,743**，但這個數字會**嚴重高估**實際的
bitwise op —— 這個階段的 `$mux` 是 word-level 且大量冗餘（1-bit mux 就有 5,482 個，
6-bit mux 2,621 個）。真正該比的是 gate-level：

```
yosys -p 'read_verilog -I. lsu/lsu_q.v; synth -top lsu_q -flatten;
          abc -g AND,OR,XOR,NAND,NOR,XNOR; opt_clean; stat'
```

**gate-level：22,968 個 2-input gate + 1,556 個 flop**（≈ 每拍 23K bitwise op）。

各子結構成本，用 ablation（把該結構的啟動條件改成 `1'b0` 再合成）量差值：

| 子結構 | gates | flops | 佔比 | 說明 |
|---|---:|---:|---:|---|
| LDQ + STQ 本體 | ~8,740 | ~730 | 38% | 4 埠 dispatch 配置 + 2 埠 dealloc + 2-of-32 埠仲裁 |
| **mem-event side FIFO** | ~6,020 | 208 | 26% | 32 × 6 bit，4 push + 4 pop 埠 |
| **MSHR** | ~4,460 | 352 | 19% | 8 entry × 26 bit、2 配置埠、8 個 8-bit 倒數器 |
| rob CAM | ~2,140 | 54 | 9% | 2 埠 × 32 entry × 6-bit 比較 |
| L1-hit 延遲線 | ~1,160 | 288 | 5% | 16 slot × 18 bit 移位 + 2 個可變位置插入 |
| **disambiguation** | **~450** | **16** | **2%** | LFSR + 衝突判定 + `wid` 比較 |

**要回答監督者的三個問題：**

1. **disambiguation 不是成本來源（~450 gate，2%）。**
   這裡**沒有 16×16 的 age matrix**。每條 load 只記一個 4-bit 的 STQ index
   (`wid`)，解鎖時只跟「本拍 dealloc 的 2 個 store index」比 —— 全部就
   16 entry × 2 埠 = 32 個 4-bit 比較器。擔心的比較器矩陣不存在。

2. **LDQ/STQ 的欄位沒有過寬**（27 / 17 bit，其中 12 bit 是 `rob`+`prf`，
   介面要求的）。貴在**埠數**：32 個 entry 每個都要能被 4 個 dispatch lane 寫、
   被 2 個仲裁埠讀。這是 4-wide OOO LSU 的本質成本，不是欄位問題。
   而且這些 cell 幾乎全是 1-bit AND/OR/MUX，結構完全規則 —— bit-slice 友善。

3. **最大的單一項目是 mem-event side FIFO（26%），而它是 v2 才加的。**
   深度 32 是為了安全（`DQ_N=16` + rename 4 + dispatch 4 = 24 是分析下限）。
   **降到 24 可省約 1,500 gate**，但失步會造成 mem event 對錯指令 →
   資料錯誤而不是效能誤差。請監督者裁決是否要壓。
   （更好的做法是把 6-bit mem event 直接塞進 `DUOP` 的保留欄位，
   讓它跟著 uop 走過 decode queue / rename —— 那樣這 6,020 gate 幾乎全省，
   但要改 `ifc.vh` 與 Agent D，是監督者的決定。**這是最划算的一刀。**）

**其他可以瘦身的選項（請監督者裁決）：**

| 手段 | 省下 (gates) | 代價 |
|---|---:|---|
| mem event 改走 `DUOP` 保留欄位，移除 side FIFO | ~6,000 | 要改 `ifc.vh` + Agent D；`lsu_q` 埠列再改一次 |
| LSQ / MSHR / 延遲線不帶 `prf`（改由 `be_eu` 用 `rob -> prf` 表查） | ~1,800 | `be_eu` 已經有 `mem_dv[rob]`，多一欄即可。低風險 |
| mem-event FIFO 32 → 24 | ~1,500 | 安全邊際變小 |
| MSHR 配置埠 2 → 1 | ~1,400 | 每拍只能開始 1 個 miss，MLP ramp 減半。**不建議**，這正是本模組要量的東西 |
| 延遲線 8 級 → 4 級 | ~500 | `lat_class=11`（prefetch hit，8 拍）被截成 4 拍 |
| LDQ/STQ dispatch 配置埠 4 → 2 | ~2,000 | **會漏配 entry 導致模型停住**，除非 `be_dispatch` 改成可部分收。**不可行** |

**關於「MSHR 只該佔 1.8% / 每拍 700 op」**：本模組的 MSHR 是 4,460 gate。
差距來自兩處，都是介面逼出來的，不是寫法問題：
- entry 寬 **26 bit 而不是預算假設的 16 bit**。多出來的是 `ldq(4)`
  （完成時要知道回填哪個 LDQ entry）、`prf(6)`（`done_prf` 要）、`dead(1)`
  （flush 語意）。真正的 MSHR 核心 `v + cnt(8) + rob(6)` 只有 15 bit。
  若採用上表的「不帶 `prf`」與「LDQ 改用 rob CAM 回填」，可以壓回 16 bit。
- **2 個配置埠**（因為有 2 個 LSU port）。
若把 MSHR 放回整個 OOO 模型的分母看：4,460 / 26K ≈ 17%，確實高於 PLAN 估的
1.8%；但 PLAN 那個估計假設的是「8 entry × 16 bit、1 埠」的裸倒數器陣列，
沒有計入回填路徑與 `done` 仲裁。
