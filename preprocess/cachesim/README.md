# BSTM 離線 cache 模擬器（`preprocess/cachesim`）

Agent B 交付物。讀一份記憶體存取串流，模擬 L1I / L1D / L2（/ 可選 L3）+ ITLB / DTLB
+ stride prefetcher，對**每一筆存取輸出一個 byte** 的 `.mem` overlay，格式由
`include/bstf.h` 的 `MEM_*` 巨集定義。

> **建模範圍（doc/PLAN.md §7.4）**：本工具輸出的是 **unloaded（無負載）延遲**。
> 刻意**不**模擬 MSHR、bank conflict、bus queueing。排隊與 MLP 由 timing model 負責。
> 這裡只回答兩件事：「命中哪一層」與「對應的 base latency class」。

---

## 1. 建置與自測

```sh
make            # -> ./cachesim
make test       # 跑 test/run_tests.sh（合成串流 + 理論值比對）
make clean
```

需求：g++ / clang++ 支援 C++17、python3（僅自測腳本用來解析 JSON）。
無任何第三方相依（JSON parser 內建於 `cachesim.cpp`）。

## 2. 介面

```
cachesim <mem-stream> <config.json> <out.mem> [--icount N] [--stats PATH] [--quiet]
cachesim --gen <kind> [args]        # 產生合成串流到 stdout（自測用）
```

* `<mem-stream>` 可用 `-` 表示 stdin；`<config.json>` 可用 `-` 表示全部採預設值。
* `--icount N`：MPKI 的分母（指令數）。沒給就依序 fallback 到
  config 的 `icount` → 串流中 ifetch 筆數 → 總存取筆數，實際用了哪一個會記在
  `.stats.json` 的 `mpki_basis`。
* stats 預設寫到 `<out.mem>.stats.json`。

### 2.1 輸入串流格式

每行一筆存取，`#` 開頭與空白行忽略，欄位以空白分隔：

```
<vaddr_hex> <size_bytes> <is_store 0/1> <is_ifetch 0/1> [pc_hex]
```

* `vaddr_hex` 可帶或不帶 `0x`。
* **第 5 欄 `pc_hex` 是選用的擴充**（原規格只有 4 欄，缺少時完全相容）。
  PC-indexed stride prefetcher 需要它；沒有 PC 時請把 `prefetcher.index`
  設成 `"page"`（見 §4）。
* 格式錯誤的行會被跳過並在 stderr 警告（前 5 筆），不中止。
* 跨 cache line 的存取（`vaddr` + `size` 橫跨 64B 邊界）會模擬**兩條 line**，
  overlay 取**較慢**的那一條；TLB 同理會查兩個 page。

### 2.2 輸出 `.mem`

無檔頭的純 byte 陣列，`out.mem` 的第 i 個 byte 對應輸入的第 i 筆有效存取。
編碼完全依 `include/bstf.h`：

| bits | 巨集 | 內容 |
|---|---|---|
| [1:0] | `MEM_LEVEL` | 0=L1 命中, 1=L2 命中, 2=L3 命中, 3=DRAM |
| [2] | `MEM_PREFETCH_HIT` | 此次命中的 line 是 prefetcher 帶進來、且是第一次被 demand 用到 |
| [3] | `MEM_TLB_MISS` | ITLB/DTLB miss |
| [7:4] | `MEM_LAT_CLASS` | `bstf_lat_table[]` 的索引 |

預設的 latency class 對應（可在 config 改）：

| 情況 | class | `bstf_lat_table` cycles |
|---|---|---|
| L1 命中 | 0 | 3 |
| L1 命中（store）| 0 | 3 —— 見 §6 註 |
| L2 命中 | 2 | 12 |
| L2 命中且是 prefetch hit | 11 | 8 |
| L3 命中 | 4 | 30 |
| DRAM | 6 | 90 |

## 3. `.stats.json`

包含：完整回填後的 config、存取計數、**每層 hit/miss/hit_rate/MPKI**、
ITLB/DTLB miss rate、prefetch 的 issued / redundant / useful / evicted_unused /
**accuracy** / **coverage**、writeback 與 L2 back-invalidation 計數、
overlay 的 level 與 latency-class 直方圖、平均 unloaded latency（cycles）。

定義：

```
accuracy = useful_prefetch / issued_prefetch
coverage = useful_prefetch / (useful_prefetch + L2_demand_miss)
           （分母 = 若無 prefetcher 需要由 L2 以下服務的 demand 次數估計值）
MPKI     = misses / (icount / 1000)
```

`redundant` = 訓練到但目標已在 L1D 或 L2，因此沒有發出的 prefetch。

## 4. `config.json`

見 `config.default.json`（可寫 `//` 與 `/* */` 註解）。所有欄位都有預設值。

```jsonc
{
  "line_size": 64,            // 所有層共用
  "page_size": 4096,
  "l1i": { "size": 32768,   "ways": 8 },
  "l1d": { "size": 32768,   "ways": 8 },      // write-back / write-allocate
  "l2":  { "size": 524288,  "ways": 8, "inclusive": true },
  "l3":  { "size": 4194304, "ways": 16, "enabled": true },
  "itlb": { "entries": 32 },  // fully-associative LRU
  "dtlb": { "entries": 64 },
  "prefetcher": {
    "enabled": true, "entries": 64, "degree": 2, "distance": 1,
    "min_conf": 2, "index": "pc"              // "pc" | "page"
  },
  "latency_class": { "l1":0, "l2":2, "l3":4, "dram":6, "prefetch":11, "store_hit":0 },
  "tlb_miss_bump_class": false,               // true: TLB miss 時 class +1
  "icount": 0
}
```

限制：`size / (ways * line_size)` 必須是 2 的冪（否則直接報錯）；任何一層可用
`"enabled": false` 關掉。

置換策略一律 **LRU**。L1D 是 **write-back + write-allocate**；髒 line 被逐出時
會寫回 L2（並可能連鎖造成 L2 逐出）。`l2.inclusive` 為 true 時，L2 逐出會
**back-invalidate** L1I/L1D（髒的 L1D 副本改寫回 L3/DRAM）。

### Prefetcher

固定 stride 偵測器，direct-mapped 訓練表：entry 記 `last_addr` / `stride` /
confidence；連續 `min_conf` 次 stride 一致才開始發 prefetch，目標是
`addr + stride*(distance + k)`（k = 0..degree-1）。**只訓練 data 存取，
只填進 L2**（依規格；不污染 L1D）。目標已在 L1D/L2 就丟掉並計為 redundant。

訓練表索引：`"pc"` 用串流第 5 欄的 PC（沒有 PC 欄時該筆自動退回用 page 號），
`"page"` 用 `vaddr >> log2(page_size)`。兩條交錯的 stride 串流下，PC-indexed
明顯較好（自測 `--gen mix2`：L2 hit 0.9992 vs 0.9463）。

## 5. 多配置單次掃描（`sweep`）

一次掃描同時算出多組 cache 大小的 hit rate，讓 DSE 的 cache 維度交叉積變便宜。

```jsonc
"sweep": { "level": "l2", "ways": 8,
           "sizes": [131072, 262144, 524288, 1048576, 2097152] }
```

`level` ∈ `l1i` / `l1d` / `l2`；`ways` 省略時沿用該層 baseline 的相聯度。
結果放在 `.stats.json` 的 `sweep.variants[]`（size / ways / sets / accesses /
hits / misses / hit_rate / mpki）。`out.mem` overlay 仍然只對應 **baseline** 配置。

### 原理

以 LRU 的 inclusion property 做 per-set stack distance。各 variant 的 set index
都是 line address 的低位元，所以**「最多 set 數」那個 variant 的 set 分割是所有
variant 分割的細分（refinement）**。對某 variant c，命中 ⇔
「該 line 所屬 group 中比它更近期被碰過的相異 line 數 < ways_c」。
又因為 fine set ⊆ group，`rank_fine ≤ rank_group`，所以 fine rank 已經
≥ max_ways 的 line 在任何 variant 都不可能命中 → 每個 fine list 只要保留
max_ways 筆。**總狀態量 = 最大 variant 的容量**，每筆存取成本
O(max_sets/sets_c + ways)。

自測 T7 會把 sweep 的結果與「每組配置各跑一次」逐一比對，必須**完全相等**。

### 精確性條件（重要）

1. 所有 variant **共用 line size**；本實作支援「同相聯度、不同大小」
   （也支援不同 ways，只要給定 `ways` 陣列——目前 config 介面是單一 `ways`）。
2. `level: "l2"` 時，餵給 sweep 引擎的是 L1 過濾後的序列（demand miss + L1D
   writeback + prefetch 填入）。因此 **L1 必須固定**，且必須設
   `"l2": {"inclusive": false}` —— 否則 L2 back-invalidate 會讓 L1 的行為
   反過來依賴 L2 大小，序列就不再是所有 variant 共用的了。
   同理，要比對 L2 sweep 與單獨跑的結果時 prefetcher 要關掉
   （prefetch 的「是否冗餘」判斷會查 baseline 的 L2，各 variant 不同）。
3. `level: "l1i"/"l1d"` 時餵的是完整串流，天生精確；但**下層（L2/L3）的統計
   不會跟著各 variant 變化**，`.stats.json` 的 `levels.*` 永遠是 baseline 的。
4. 一次只能掃一層。要掃 L1×L2 交叉積請跑 |L1| 次、每次帶一組 L2 sizes。

## 6. 已知限制（誠實記錄）

1. **無 MSHR、無 bank conflict、無 bus/queue、無 MLP** —— 依 PLAN 設計如此，
   延遲一律是 unloaded。Timing model 必須自己補這些。
2. **無 pipeline 回饋**：prefetcher 只被離線的 access stream 訓練，
   不受時序影響（PLAN §7.4 已列為已知準確度缺口）。
3. **只有 virtual address**：不做 VA→PA 轉換，cache index/tag 直接用 VA
   （等同 VIVT，或假設 page colouring）。TLB 只回報 hit/miss，不模擬 page walk
   的記憶體存取，也沒有 page walk cache。TLB miss 的懲罰由 `MEM_TLB_MISS` 這個
   bit 交給 timing model 加（或開 `tlb_miss_bump_class` 讓 class +1）。
4. **ifetch 也會產生一個 overlay byte**。`.bstf` 的 `mem_index_delta` 語意上
   通常只給 load/store，指令 fetch 屬於 `.fe` 串流（Agent A）。若 `.mem` 只該
   含 data 存取，請在餵進來之前先把 `is_ifetch=1` 的行過濾掉；本工具不替你決定。
5. **不寫 `bstf_hdr_t`**：輸出是裸 byte 陣列。header（含 `n_mem_access`）由組裝
   `base.bstf` 的人負責填。cache 配置指紋放在 `.stats.json`（`bstf_hdr_t` 裡
   沒有 PLAN §7.2 草稿提到的 `cache_cfg[64]` 欄位）。
6. **latency class 10（store buffer hit, 2 cycles）預設沒用到**：store 命中 L1D
   走 class 0。store buffer forwarding 屬於 LSU/timing model 的事。若要改，
   設 `latency_class.store_hit = 10`。
7. **L2 的 hit/miss 統計以 line 為單位**（L1 miss 次數），不是以「指令」為單位。
8. shared L2/L3 只模擬單執行緒、單核；沒有 coherence。
9. 吞吐量：約 **8 M 存取/秒**（單執行緒，隨機串流，含 I/O 解析）；
   開 6 組 L2 sweep 幾乎不增加成本。

## 7. 合成串流產生器（自測用）

```
cachesim --gen seq      <n> <stride> [size] [base]   # 順序 load
cachesim --gen store    <n> <stride> [size] [base]   # 順序 store
cachesim --gen loop     <footprint> <iters> [stride] [size]
cachesim --gen random   <n> <range> [seed]
cachesim --gen conflict <n_lines> <line_stride> <iters>
cachesim --gen stridepf <n> <stride> [size]          # 附 PC 欄
cachesim --gen mix2     <n> <strideA> <strideB>      # 兩條交錯的 PC 串流
```

## 8. 自測涵蓋與實測數字（`make test`，32 項全過）

| 測項 | 串流 | 理論值 | 實測 |
|---|---|---|---|
| T1 | 8B 順序掃描（8 次/line） | L1D 7/8 = 0.875；全部 compulsory miss → L2 = 0 | 0.875 / 0.0 |
| T1 | overlay 大小與 level 分佈 | 80000 byte，L1 70000 / DRAM 10000 | 完全相符 |
| T2 | 16 KB 迴圈 ×20（放得進 32 KB L1D） | (5120−256)/5120 = 0.95 | 0.95 |
| T3 | 256 KB 迴圈 ×10 | L1D 循環 thrash = 0；L2 (40960−4096)/40960 = 0.9 | 0.0 / 0.9 |
| T4a | 8 條 line 全部撞同一個 set（8-way） | 0.9 | 0.9 |
| T4b | 9 條 line 撞同一個 set → LRU thrash | L1D 0.0；L2 0.9 | 0.0 / 0.9 |
| T5 | 64B stride + prefetcher | L2 hit ≈ 1、accuracy ≈ 1 | 0.9998 / 0.9999，coverage 0.9998 |
| T5 | 平均 unloaded latency 下降 | 90 → 8 cycles | 90.0 → 8.02 |
| T6 | 64 MB 隨機 | L1D ≈ 0、DTLB miss ≈ 1 | 0.00045 / 0.9959 |
| T7 | sweep（6 組 L1D + 5 組 L2）vs 各自單跑 | 必須逐一相等 | 11/11 完全相等 |
| T8 | 用 `bstf.h` 巨集解回 overlay byte | 與 stats 直方圖一致 | 一致 |

## 9. 檔案

```
cachesim.cpp              全部實作（內建 JSON parser、無第三方相依）
Makefile                  make / make test / make clean
config.default.json       baseline 配置（demo 規格）
config.sweep.example.json 多配置掃描範例
test/run_tests.sh         自測
build/                    自測產物（make clean 會清掉）
```

唯一寫入範圍是 `preprocess/cachesim/`；`include/bstf.h` 只讀不改。
