# BSTM runtime（Agent G）

模型無關的那一半：把 `.bstf` 餵進 bit-sliced 模型、把 bit-sliced counter 抽回來。
對應 PLAN 第 8 章。**不含任何模型知識** —— 模型透過 `bstm_abi.h` 熱插拔。

```
bstm_types.h   vec_t / LANES / 欄位寬度（要與 model/ooo/common/ifc.vh 對齊）
bstm_abi.h     runtime <-> 編譯出來的模型 的介面（PLAN §13.2 的具體化）
bstf_reader.*  mmap .bstf/.fe/.mem/.fe.wp/.mem.wp/.imem + 三個游標 + wrong-path
transpose.*    64x64 bit transpose（Hacker's Delight + AVX2/BMI2 手寫優化）
refill.*       scalar/vector 邊界（PLAN §8.5）
batch.*        instance -> lane 打包（PLAN §8.2）
pool.*         worker 執行緒池 + work stealing（§8.3）與主迴圈（§8.4）
stats.*        bit-sliced counter -> per-lane uint64 -> CSV（§8.6）
```

```sh
make            # -> libbstm.a
make test       # 跑 ../../test 的單元測試
make bench      # transpose / refill 的效能數字
ARCH= make      # 關掉 AVX2/BMI2，退回純量版（換平台時用）
```

---

## 1. 資料流

```
 .bstf (mmap, 唯讀, 所有 lane 共用)
      │  bstf_cursor_t × 64       每 lane 一組 (rec, fe, mem) 游標
      ▼
 bstm_refill_gather()             每 lane 一次抓 REFILL_M 筆，解碼成 48-bit entry
      │  scalar staging[64][32]   ← 這一層才會碰 mmap / 分支 / shadow 切換
      ▼
 bstm_refill_window()             取最前面 W=4 筆 -> 每 lane 打包成 3 個 word
      │  3 次 64x64 bit transpose
      ▼
 bstm_fbwin_t                     fb_valid / fb_duop / fb_fe_event / fb_mem_event
      │
      ▼  model->eval()            純 bitwise，無分支
 bstm_out_t                       fb_take[3] / fb_redirect / fb_redir_shadow
      │
      ▼  bstm_refill_consume()    unpack take(3 bit) -> 推進 scalar 游標
```

### 為什麼 window 每 cycle 重建，而不是做 bit-sliced 的 barrel shift

兩種做法的每-cycle 成本都正比於 `W × ENTRY_W` 個 bit：
重建要 3 次 64×64 轉置；shift 要對一個深度 ≥ 2W 的 bit-sliced buffer 做
3 級 barrel（shift by 1/2/4）。實測重建略便宜，而且沒有「每 lane 插入位置
不同」的問題（那要另一層 mux），程式碼也短很多。

`REFILL_M` 攤銷的是 **scalar 端** 的成本（mmap 隨機存取、欄位解碼、shadow
分支），不是轉置成本 —— 轉置正比於記錄數，改 `REFILL_M` 不會變。
實測見下面「REFILL_M 掃描」。

---

## 2. `.bstf` 格式的解讀約定

`include/bstf.h` 沒有寫死的地方，本 reader 採用的約定（要改請通知監督者）：

| 項目 | 本 reader 的約定 |
|---|---|
| 記錄長度 | **一律用 `hdr.rec_bytes`**，不用 `sizeof(bstf_rec_t)`；只要 `rec_bytes >= sizeof` 就能讀 |
| fe / mem 索引 | CONTRACT §5：`idx(i) = Σ_{j≤i} delta(j) − delta(0)`。實作上就是「cursor 從 0 起跳、永遠不加 delta(0)」，所以 `.fe` 的 `delta(0)=1` 與 `.mem` 的 `delta(0)=0` 都會得到 `idx(0)=0` |
| 「這筆要不要消耗一個 `.mem` entry」 | **只看 `uop_class ∈ {LOAD, STORE, AMO}`**（`bstf_rec_has_mem()`）。**不能看 `mem_index_delta`** —— 第一筆存取的 delta 是 0，跟非記憶體指令無法區分。非 mem 指令的 `fb_mem_event` 一律餵 0（`bstf_mem_event_of()`） |
| `shadow_off` | 相對 **`.bstf` 檔案起點** 的位元組偏移（與 `hdr.shadow_offset` 同一個基準） |
| shadow 期間的 fe/mem | 游標**凍結**（v2 A1/B2：`.fe`/`.mem` 只含 correct path），事件改讀 `.fe.wp` / `.mem.wp` |
| `.fe.wp` 索引 | `(shadow_off − hdr.shadow_offset) / rec_bytes` 再加上在該 shadow 走了幾筆。<br>裁決寫的是 `[k*D, (k+1)*D)`；只要 shadow 區是每次誤預測連續 D 筆，兩者等價，而且這樣不需要知道 D |
| overlay 健全性 | `.fe` 大小 == `hdr.n_fe_blocks`、`.mem` == `hdr.n_mem_access`、`.fe.wp` == `hdr.shadow_bytes / rec_bytes`；不符直接報錯（`bstf_open_ex(..., BSTF_LAX)` 可放寬） |
| `.meta.json` | 選讀，`bstf_meta_get()` 取 flat key，用來驗證配置指紋 |
| `.imem` | 只 mmap 保留，demo 不接線 |

游標同時提供兩種模式：`rec_byte`（位元組，權威，未來換變長格式仍有效）與
`bstf_cur_index()`（記錄索引，只在定長 + correct path 時有意義）。

### `fb_redirect` / `fb_redir_shadow` 的語意（**top.v 沒有規定，本 runtime 自訂**）

- `redir_shadow = 1`：跳到「游標**當下或之後**第一筆帶 shadow 的記錄」的 wrong-path，
  staging 截到含該分支為止（分支之前的正確路徑指令不丟）。
- `redir_shadow = 0`：丟掉還沒消耗的 wrong-path，回到該分支的下一筆。

**為什麼是往前找**：`fe_event` 是 per-fetch-block 的，模型看到 `FE_REDIRECT`
時觸發的分支通常是該 block 的最後一條、還沒被消耗。第一版往回找「最近一筆
已消耗且有 shadow 的記錄」，實測會讓 correct-path 游標倒退而**永遠跑不完**
（5,000,000 cycle 還在原地，78% 的 retire 是 wrong-path）。往前找可以保證每次
redirect 至少跨過一個分支。**這個語意應該由 top.v 或 CONTRACT 規定** —— 已回報。

---

## 3. 模型 ABI（`bstm_abi.h`）

```c
const bstm_model_desc_t *describe(void);
void *state_alloc(void);  void state_free(void *);
void  init (void *st, const uint32_t cfg[lane][BSTM_CFG_N], uint64_t lane_mask);
void  eval (void *next, const void *cur, const bstm_fbwin_t *win, bstm_out_t *out);
void  extract(const void *st, uint64_t counters[lane][n_counters]);
uint64_t done_mask(const void *st);           /* 可回 0 */
void  set_enable(void *st, uint64_t mask);    /* 可為 NULL */
```

`cfg` 的欄位順序 = `CONTRACT §3`（`bstm_cfg_names` / `bstm_cfg_widths`）。
參考實作見 `test/mock/mock_ooo_bs.c`。

---

## 4. 實測數字（i7-12700，~4.4 GHz，gcc 13.3，`-O3 -march=x86-64-v3`）

### transpose

| | ns/次 | 說明 |
|---|---|---|
| `bstm_transpose64`（原版 HD 迴圈） | **228** | `k = (k+j+1)&~j` 相依鏈長，編譯器展不開 |
| `bstm_transpose64`（本版，兩層迴圈 + AVX2） | **45.8** | 0.71 ns / 64-bit row、0.011 ns / bit-lane |
| `bstm_pack_lanes(48 bit)` | 48.3 | 含複製 |
| `bstm_unpack_lanes_small(3 bit)`（天真 64×n 迴圈） | 98 | 比整個 64×64 轉置還慢 |
| `bstm_unpack_lanes_small(3 bit)`（BMI2 `pdep`） | **12.6** | `fb_take` 每 cycle 要做一次 |

做了什麼：
1. 把 HD 的跳躍式 `k` 迴圈改成兩層迴圈（外層 block、內層連續），
   讓 `j ≥ 4` 的四輪能用 AVX2 一次處理 4 個 row。→ **5.0×**
2. 小 n 的 pack/unpack 改用 BMI2 `pdep`/`pext`（一次攤 8 個 lane）。→ **7.8×**
3. `bstm_refill_window` 改用三個連續陣列直接原地轉置，省掉 stride gather。

（HD 原版是 MSB-first 的 column 編號，直接照抄會得到**反對角轉置**。
本版把 shift 換到另一個運算元，得到標準的 `B[i] bit j == A[j] bit i`。
`test/unit/t_transpose.c` 有 4096 個逐 bit 的檢查。）

### refill 的攤銷成本

每 lane 固定 `take = 1..4`（平均 2.5，最壞情況），64 lane，40 萬 target cycle：

| | ns / target cycle | host cycle / target cycle | ns / instance-cycle |
|---|---|---|---|
| window（3 次轉置 + 打包） | 299 | 1,300 | 4.67 |
| consume + scalar gather | 521 | 2,260 | 8.14 |
| **refill 合計** | **820** | **3,469** | **12.8** |
| mock 模型 eval（對照） | 377 | 1,593 | 5.9 |

→ 以這個 mock 當分母，refill 佔 68%。真模型大得多（`ooo_top` 至少幾萬個 cell），
所以實際比例會低很多；**但 refill 的絕對成本是固定的，它是 IPC 的地板**。

### REFILL_M 掃描

| REFILL_M | ns / target cycle | 每 lane 幾 cycle gather 一次 |
|---|---|---|
| 8 | 750 | 6.1 |
| 16 | 757 | 14.4 |
| 32（預設） | 782 | 30.3 |
| 64 | 669 | 62.6 |
| 128 | 659 | 126.3 |

**幾乎是平的**（±12%）。結論：瓶頸不是 per-call 的 overhead，而是
**每筆記錄的解碼成本**（`bstf_rec_to_duop` + 游標推進），那個正比於記錄數，
`REFILL_M` 攤銷不掉。

### 給專案的建議（尚未實作）

1. **離線預解碼 trace**：把 `.bstf` 在前處理階段就轉成 48-bit 的 entry 陣列，
   runtime 的 gather 就退化成 `memcpy`。以上面的拆解看，可望省掉 ~500 ns / cycle
   中的大部分。代價是多一份 6 byte/指令 的檔案（比 16 byte 的 `.bstf` 還小）。
2. **共用解碼快取**：PLAN §8.2 建議「同 simpoint、不同 config」同 batch，
   也就是 64 個 lane 讀**同一份** trace，只是游標不同。目前每個 lane 各解碼一次；
   一份 LRU 的已解碼視窗可以把 64× 的重複解碼砍掉。
3. counter 數量要克制（PLAN §8.6 已提）：光 11 個 48-bit counter 的 ripple-carry
   在 mock 裡就佔了 eval 的大半。

（監督者已採納第 1 點，列為 PLAN 的後續優化項；這一輪不做，因為會再動 trace 格式。）

### 端到端

200 個 instance（100 config × 2 simpoint）→ 4 個 batch → 8 worker：
**~390 M instance-cycles/s**，輸出 `build/sweep_mock.csv`。

---

## 5. 已知限制

- `vec_t` 固定 `uint64_t`、`BSTM_LANES` 固定 64。要 128/512 lane（NEON/AVX-512）
  要改 `bstm_types.h` 並把 `transpose.c` 的 pack/unpack 泛化。
- `bstf_cur_seek_index()` 是 O(n)（要累加 delta）。只在 setup 用，主迴圈不要呼叫。
- work-stealing deque 用 per-deque mutex，不是 lock-free。batch 的粒度是秒級，
  爭用可忽略；要更細粒度再換 Chase-Lev。
- 輸出是 CSV。PLAN 的長期目標是 Parquet。
- 「lane 跑完就凍結」是靠 `model->set_enable()`；模型沒實作 `set_enable` 的話，
  已結束的 lane 會繼續累加 counter（runtime 仍會停止餵 trace）。
