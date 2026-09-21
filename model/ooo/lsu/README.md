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

## 5b. 尺寸參數化（CONTRACT v7 §0：禁止寫死數字）

本模組**沒有任何硬編的尺寸**。所有宣告都從 `` `XXX_N `` / `` `XXX_W `` 推導：

| 來源 | 用在哪 |
|---|---|
| `` `ROB_W `` | `ld_rob` `st_rob` `ms_rob` `hl_rob` `pq_rob` `d_rob` `sr0/1` `g_rob0/1` `c_rob` 及所有切片 |
| `` `PRF_W `` | `ld_prf` `st_prf` `ms_prf` `hl_prf` `d_prf` `g_prf0/1` `c_prf` |
| `` `LDQ_N `` `` `STQ_N `` | `LDN` `STN`、所有佇列陣列 |
| `` `LSQ_W `` | index（`ld_head/tail` `ld_wid` `ms_ldq` `hl_idx` `dh` `a_ldt` `c_idx` …）|
| `` `LSQ_W + 1 `` | **佔用計數**（`ld_cnt` `st_cnt` `a_ldc` `a_stc` `tmp5`）—— 要裝得下 MAX 本身 |
| `` `MSHR_N `` / `` `MSHR_W `` | `MSN` / `ms_sel` 與 MSHR index 比對 |
| `$clog2(...)` | 本模組自有結構（延遲線 `HD/HDW/HS/HSW`、請求緩衝 `PQN/PQW/PQCW`、mem-event FIFO `MQN/MQW/MQCW`）|

零擴展與常數也都推導：`{{(LQCW-2){1'b0}}, ndl_ld}`、`{{(LQCW-LQW){1'b0}}, dh}`、
`TGT_MAX = {HDW{1'b1}}`、`LQ_MINV`、`MQ_MAXV`…；所有遞增改成 `+ 1'b1`（寬度無關）。

`ld_rob`/`ld_prf`（及 st/ms/hl 的對應項）**必須分開宣告** —— 它們原本共用一個
`[N*6-1:0]`，ROB 與 PRF 寬度分家之後合併宣告就是錯的。

**不隨尺寸變動的常數：** mem-event payload `MEV_W = 6`（`{lat_class(4), level(2)}`，
由 `include/bstf.h` 的 `MEM_*` 定義），以及 `lat_lut` 的 4-bit class。已經具名並加註解。

### 驗證矩陣（`test/run_tests.sh`，5 組尺寸）

| 變體 | ROB | PRF | LSQ | lint | yosys | 功能 | cells | flop bits |
|---|---|---|---|---|---|---|---:|---:|
| v7-base | 64/6 | 64/6 | 16/4 | 0 warn | 0 prob | PASS | 12,743 | 1,556 |
| rob128 | **128/7** | 64/6 | 16/4 | 0 warn | 0 prob | PASS | 12,748 | 1,620 |
| prf256 | 64/6 | **256/8** | 16/4 | 0 warn | 0 prob | PASS | 12,748 | 1,668 |
| rob128-prf256 | **128/7** | **256/8** | 16/4 | 0 warn | 0 prob | PASS | 12,748 | 1,732 |
| lsq8 | 128/7 | 256/8 | **8/3** | 0 warn | 0 prob | PASS | 8,860 | 1,294 |

### flop 數推導對照（逐項對得上，代表寬度真的打通、沒被靜默截斷）

**每增加 1 個 `` `ROB_W `` bit：+64 flop**
= LDQ 16 + STQ 16 + MSHR 8 + 延遲線 16 + 請求緩衝 8 = **64** 個 `rob` 欄位。
（`d_rob` 是組合輸出，不算 flop。）實測 1,620 − 1,556 = **64** ✓

**每增加 1 個 `` `PRF_W `` bit：+56 flop**
= LDQ 16 + STQ 16 + MSHR 8 + 延遲線 16 = **56** 個 `prf` 欄位。
（請求緩衝只存 rob。）實測 (1,668 − 1,556)/2 = **56** ✓

**兩者可加：** 1,556 + 64 + 112 = **1,732** ✓（與 rob128-prf256 實測完全相同）

**LSQ 16/4 → 8/3：−438 flop**

| 項目 | 16/4 | 8/3 | Δ |
|---|---:|---:|---:|
| LDQ entry = v,ar,st,blk,dn(5)+rob(7)+prf(8)+lvl(2)+lc(4)+wid(LSQ_W) | 16×30=480 | 8×29=232 | −248 |
| STQ entry = v,ar,st,dn,noacc(5)+rob(7)+prf(8) | 16×20=320 | 8×20=160 | −160 |
| `ms_ldq` (MSHR_N × LSQ_W) | 32 | 24 | −8 |
| `hl_idx` (HS × LSQ_W) | 64 | 48 | −16 |
| `ld/st_head`,`ld/st_tail` (4 × LSQ_W) | 16 | 12 | −4 |
| `ld_cnt`,`st_cnt` (2 × (LSQ_W+1)) | 10 | 8 | −2 |
| | | | **−438** |

實測 1,732 − 1,294 = **438** ✓

**成本結論：ROB / PRF sweep 對本模組幾乎免費**（每 bit 64 / 56 個 flop，
gate 數在雜訊內：12,743 → 12,748）。兩者在 `lsu_q` 都不是結構性維度。

---

## 5c. `cfg_*` 的可表示下限（契約層面的觀察，請監督者裁決）

CONTRACT §3 寫 `cfg_ldq_entries` 範圍是 ``1..`LDQ_N``，但 CONTRACT v2 同時裁決
**所有 `*_ready` 是 all-or-nothing、不支援 partial accept**。這兩條有衝突：

一組 dispatch 最多有 `` `W `` 條記憶體 uop，而 `be_dispatch` 要嘛整組收、要嘛整組不收。
所以只要 `cfg_ldq_entries < ` `` `W ``，`lsq_full` 就永遠拉高 → 模型不前進。
**小於 `` `W `` 的 LSQ 在這個協定下無法表示。**

本模組的處理：把 `ld_max` / `st_max` 夾在 `` [`W, `LDQ_N] `` —— 夾住而不是掛掉，
這樣 CONTRACT v7 §10.1 要求的「cfg 掃到最小值」不會讓模型停住（實測 cfg=1 與 cfg=4
行為相同、都正常前進）。

**建議二選一：**
1. §3 把 LDQ/STQ 的範圍改成 ``\`W..`LDQ_N``，或
2. `lsq_full` 改成能表達 partial accept（要動 `be_dispatch` 與 ready 語意）

---

## 5d. 跨模組不變量：mem-event side FIFO 不得溢位【重要】

side FIFO 深度 `MQN = 32`。**在 `fb_take` 與 `ds_valid` 之間同時在飛的記憶體 uop
數量必須 < 32**，否則 push 會被丟掉，之後每一次 pop 都錯位 —— 後續所有 load
都拿到別人的 cache 事件。**這是資料錯誤，不是效能誤差。**

目前的邊界：`fe_front` 的 `DQ_N = 16` + rename/dispatch 各 `` `W `` 條 ≈ 24 < 32，
有 8 條的餘裕。**但如果 Agent D 加深 decode queue，這個不變量會靜默失效。**

這正是 CONTRACT v7 §10 講的那類「容量不足、lint 抓不到」的錯誤，所以我為它寫了
**專門的偵測與負測**（見 §8 的 T2 / T6）：
- T2 斷言「每個記憶體 uop 的實測延遲 >= 它的 base latency」。錯位會讓一條 DRAM
  load（90 拍）拿到 L1-hit 事件（3 拍）而提早完成 → 立刻被抓到
- T6 是負測：刻意把 fetch→dispatch 距離拉到 12 級（遠超 32），斷言 T2 **必須**報錯
  （實測 latviol = 222~235）。這保證 T2 不是一個永遠成立的空檢查

**根本解**仍然是先前提過的：把 6-bit mem event 塞進 `DUOP` 的保留欄位讓它跟著 uop
走過 decode queue，side FIFO 就整個消失（同時省掉約 6,000 gate，見 §7）。
那需要改 `ifc.vh` + Agent D，是監督者的決定。

---

## 8. 自測（`test/`）

```bash
model/ooo/lsu/test/run_tests.sh [scratch_dir]     # 預設 /tmp/agentf_lsu_test
```

對 §5b 表格裡的 5 組尺寸，各跑 **lint + yosys + 功能測試**。
`test/gen_layout.py` 從 `common/ifc.vh` 抽欄位佈局產生 `layout.h`，
所以 bit 佈局的唯一真相留在 `ifc.vh`，測試不自己複製一份。

| 測項 | 內容 |
|---|---|
| T1 | 每個記憶體 uop 恰好收到一次 `done`、不卡死（含 AMO 只回報一次） |
| T2 | **mem-event 對齊**：實測延遲不得低於該 uop 的 base latency |
| T3 | **MSHR 是 MLP 上限**：全 DRAM 時 miss 吞吐 <= `min(MSHR_N, LDQ_N) / latency` |
| T4 | `cnt_st_mshr` 語意：全 L1 hit 時為 0；全 DRAM 且 `LDQ_N > MSHR_N` 時 > 0 |
| T5 | `cfg_ldq/stq` 在 **最小 / 中間 / MAX** 三點都要前進，且吞吐不隨 mask 變大而劣化 |
| T6 | **負測**：刻意讓 side FIFO 溢位，斷言 T2 必須報錯（證明 T2 有牙齒）|

實測（v7-base）：

```
cfg=1    cycles=8583  done=2912  thr=0.339/cyc  st_mshr=0     latviol=0
cfg=8    cycles=3339  done=2912  thr=0.872/cyc  st_mshr=0     latviol=0
cfg=16   cycles=3224  done=2912  thr=0.903/cyc  st_mshr=0     latviol=0
all-DRAM cycles=6692  miss=565   miss_thr=0.0844/cyc  ceiling=0.0889  st_mshr=5581
negative(deep fetch pipe) latviol=235   <- T6 期望 > 0
```

兩個值得記錄的觀察：

1. **T3 是本模組存在意義的直接量測**。全 DRAM 時 miss 吞吐 0.0844/cyc，
   緊貼 8 MSHR / 90 拍 = 0.0889 的理論上限 —— MSHR 確實在當 MLP 的閘門。
2. **`LDQ_N = 8` 時 `cnt_st_mshr` 變成 0**，因為 LDQ 先滿，MSHR 永遠吃不滿。
   也就是說 **MLP 的瓶頸是 `min(MSHR_N, LDQ_N)`**，掃 MSHR 數量時若不同時放大
   LDQ，會量到一條假的平坦曲線。做 MSHR sweep 的人要注意這點。

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
