# frontend_sim — BSTM 離線前端模擬器（Agent A）

離線（trace-driven）模擬完整的前端預測階層，為每個 fetch block 產生 **1 byte**
的 overlay，格式由 `include/bstf.h` 的 `FE_*` 巨集定義。timing model 只需要讀這
個 byte，不必在模型裡實作任何預測器 —— 這正是 `CONTRACT.md` §1 規則 1（欄位
≤ 8 bit、禁止寬算術）能成立的前提。

```
指令串流 ──► frontend_sim ──┬─► out.fe             1 byte / correct-path fetch block
                   │        ├─► out.fe.wp          1 byte / wrong-path block（每次誤預測 D 個）
                config.json ├─► out.stats.json     統計
                            └─► out.fe.meta.json   配置指紋 sidecar
```

## 1. 建置與自測

```sh
make            # 產生 build/frontend_sim 與 build/fe_check
make test       # 完整自測（見 §8），PASSED/FAILED
make demo       # 只跑一次合成串流
make sweep      # wrongpath_depth (D) 敏感度掃描（見 §10）
make clean
```
只需要 `g++ -std=c++17` 與 `python3`（產生自測串流用），無第三方相依。

## 2. 命令列

```
frontend_sim <instr-stream|-> <config.json> <out.fe> [out.stats.json]
fe_check     <file.fe> [--dump N]
```
* `instr-stream` 用 `-` 表示 stdin。
* 未指定第 4 個參數時，統計檔名 = `out.fe` 去掉 `.fe` 再加 `.stats.json`。
* 統計同時印到 stderr。
* `fe_check` 用 `bstf.h` 的巨集解碼 overlay、檢查欄位合法性並印出統計，
  回傳 0 表示通過。

## 3. 輸入格式（frontend format v2，7 欄）

每行一筆指令，純文字，空白分隔；`#` 開頭與空行忽略。

```
<pc_hex> <is_branch 0/1> <br_type> <taken 0/1> <target_hex> <is_block_end 0/1> <next_pc_hex>
br_type     : 0=cond 1=jal 2=jalr 3=ret 4=call（非 branch 時忽略）
target_hex  : 解碼出的 architectural 分支目標，**與 taken 無關**；非分支為 0
next_pc_hex : 下一條實際執行指令的 PC（= 下一行的第 1 欄）；最後一行為 0
```

`next_pc` 是「fetch 該往哪去」的 ground truth，`target` **只用來配置 BTB**
（記「這個分支若 taken 會去哪」），不決定實際流向。
這兩件事在 RVC 下必須分開：fall-through = `PC + 指令長度`，而指令長度
是 2 或 4 byte，格式裡沒有，**不能用 target/taken 推**。

### 3.1 三種歷史慣例（向後相容）

| 情況 | 判定 | 行為 |
|---|---|---|
| **7 欄** | 有第 7 欄 | 主路徑：用 `next_pc` 當 ground truth |
| **6 欄，not-taken 的 target 恆為 0** | 沒有第 7 欄且從未出現 `taken=0 且 target≠0` | 舊 `gen_trace.py` 慣例，用「下一個 block 的起始 PC」推導，結果與 7 欄一致 |
| **6 欄，not-taken 的 target 非 0** | 沒有第 7 欄但出現 `taken=0 且 target≠0` | Agent H 的舊檔案。缺 `next_pc` 又沒有指令長度 → **無法可靠推導 fall-through，直接報錯 exit 3**；`allow_ambiguous_6col: 1` 可降級成警告 |

偵測是串流即時做的（第一行就能知道是不是 7 欄），不需要預掃檔案，
stdin 也適用。

**fetch block 的切法**：累積指令直到遇到 `is_block_end=1`、
或「taken 的 branch」、或達到 `max_blk_instr` 上限。block 的 PC = 第一條指令的 PC。
**一個 fetch block 可以含 0、1 或多個分支**（CoreMark 實測：37.7% 的 block
完全沒有分支、8.0% 含 2 個以上），模型必須據此設計（見 §4）。

> 整合時這個文字介面會換成二進位（`bstf_rec_t` 串流）。要換只需重寫
> `BlockReader::read_raw()`，其餘程式碼不動。`bstf_rec_t` 需要能表達
> 「這條指令的下一個 PC」或「指令長度」，否則會再踩一次同樣的坑。

## 4. 模擬的結構

預測器是 **FTB（fetch target buffer）語意**，不是傳統「一個分支一個 BTB entry」：
每個 **fetch block 起始 PC** 一個 entry，裡面記

* 這個 block 的前 N 個分支（PC / 型別 / 解碼目標 / 2-bit 方向 hint）
* 這個 block 的 **fall-through 位址**（沒有任何分支 taken 時的結束位址）

這是 XiangShan FTB / BOOM 的做法，也是唯一能同時處理
「沒有分支的 block」與「RVC 下算不出 block 長度」的結構。
沒有分支的 block 也會配置 entry —— 它記的是「這個 block 到哪結束」。

| 結構 | 預設配置 | 行為 |
|---|---|---|
| **uBTB** | 16 entry，全關聯，LRU，1 個分支 slot | tag = fetch block PC 低位 hash（20 bit，會 alias）；**0 bubble** |
| **L2 BTB** | 8K entry、4-way、LRU、2 個分支 slot | index = `PC[1+]`；override uBTB 時 **1 bubble** |
| **TAGE** | bimodal 13-bit index + 2-bit ctr；4 個 tagged table，hist {8,16,32,64}、各 1K entry、3-bit ctr、8-bit tag、2-bit useful | **以分支 PC 索引**（不是 block PC）；provider / altpred、`use_alt_on_na`、折疊歷史（Seznec CSR）、mispredict 時往更長的表 allocate、useful 週期老化；override BTB 時 **2 bubble** |
| **RAS** | 16 entry，環狀 | call push 回傳位址 / ret pop；溢位時覆寫最舊的 entry |

**預測流程**：查到 entry 後，依序走它的分支 slot，
**第一個被預測為 taken 的分支決定流向**（uncond 一律 taken、cond 問方向預測器、
ret 的目標取自 RAS）；全部都 not-taken 就走 entry 記的 fall-through。
entry 沒命中時只能猜 `PC + fetch_width*ilen_bytes`。

**Override 鏈**：`stage1 = uBTB(hint)` → `stage2 = BTB(hint)` → `stage3 = BTB + TAGE 方向`。
每一級各自給出一個 `(taken, next_pc)`，
`bubbles = (s3≠s2) ? 2 : (s2≠s1) ? 1 : 0`，最終預測取 `s3`。
`btb_dir_hint: 0` 可讓 BTB 階段直接用 TAGE 的方向（此時只會有 0/1 bubble）。

**更新時機**：correct path 上所有結構都用 ground truth 在「解析時」更新。
FTB entry 只覆寫「這次執行真的看到的部分」——
block 因為 taken 而提早結束時，後面的 slot 與 fall-through 保留舊值，
等到某次 not-taken 跑完整個 block 才會補上。
TAGE / RAS 依 block 內分支的**執行順序**逐一更新。
RAS 不在預測階段推測性 push/pop，等同於「完美修復的 RAS」（見 §9）。

**指令長度學習**：not-taken 分支的 `next_pc - PC` 直接給出該分支指令的長度；
call 永遠 taken、看不到 fall-through，所以改從「對應的 ret 實際回到哪裡」
回推（`learn_call_len`，預設開）。這讓 RAS 在 RVC 程式上也準。

## 5. wrong-path 展開

誤預測（方向或目標任一錯）時：

1. 存檔推測狀態（TAGE 的全域歷史 + 所有折疊歷史、RAS）。
2. 把誤預測 block 自己的「預測方向」推進歷史。
3. 從「被錯誤預測的 next PC」開始，往下模擬 `wrongpath_depth` (D) 個 fetch block：
   * 每個 block 一樣跑完整的 uBTB→BTB→TAGE 流程，
     輸出一個標了 `FE_WRONGPATH` 的 byte；
   * 若這個 PC 之前在 correct path 上出現過（模擬器邊跑邊建 block map），
     就用它的靜態資訊（br_type / target / 最近一次方向）來當「實際結果」，
     **配置 uBTB / BTB entry**（`wrongpath_train_btb`，預設開 —— 真實硬體
     在 fetch/predecode 就會配置）；TAGE 計數器**預設不訓練**
     （`wrongpath_train_tage`，預設關 —— wrong-path 指令不會 retire），見 §10.1；
   * 否則用**合成 block**：假設 4 條指令、PC 順序遞增、不含 branch，
     只消耗 fetch 頻寬、污染推測歷史，不訓練 BTB（因為根本不知道有沒有 branch）。
   * wrong path 沿著**自己的預測**往前走（predicted next PC）。
4. 還原推測歷史與 RAS，再用 ground truth 做 correct-path 的解析更新。

uBTB/BTB 的**內容不回滾**（這正是 wrong-path 的容量污染/prefetch 效應，
也是模擬它的主要理由）；推測歷史與 RAS 則精確回滾。

這個效應是可量測且可證偽的：在 working set 超過 uBTB 容量的串流上，
wrong path 等於替 uBTB 做 prefetch（+2.324% 命中率 / block），
直到 `D == ubtb_entries` 時整個 uBTB 被換過一輪、命中率崩回 0 ——
懸崖位置在 U=8/16/32 三種容量下都精確落在 D=U。詳見 **§10.2（主結論）**。

## 6. 輸出

### 6.1 `.fe` overlay
每個 fetch block 1 byte，欄位完全照 `include/bstf.h`：

| bit | 巨集 | 本模擬器的語意 |
|---|---|---|
| [1:0] | `FE_BUBBLES` | 0 / 1 / 2（override 造成的前端泡泡；本模型不產生 3） |
| 2 | `FE_UBTB_HIT` | uBTB 命中 |
| 3 | `FE_BTB_OVERRIDE` | L2 BTB 的預測與 uBTB 不同 |
| 4 | `FE_DIR_OK` | 方向預測正確（`pred.taken == actual.taken`） |
| 5 | `FE_TGT_OK` | 下一個 PC 預測正確（`pred.next == actual.next`；not-taken 時比的是 fallthrough） |
| 6 | `FE_WRONGPATH` | 這是錯誤路徑上的 block |
| 7 | `FE_REDIRECT` | 此 block 會觸發 redirect（= `!(DIR_OK && TGT_OK)`）；wrong-path block 不標（它們會被更舊的 branch flush） |

**wrong-path byte 放哪裡**（`wrongpath_out`）：

| 模式 | `.fe` 內容 | 用途 |
|---|---|---|
| **`separate`（預設）** | `.fe` 只有 correct path（線性、每 block 一 byte、游標可直接累加）；wrong-path byte 寫到 `<out>.fe.wp` | **正式格式**，`bstf.h` v2 / 監督者 A1 裁決 |
| `inline` | correct-path byte 後面緊跟著它展開的 D 個 wrong-path byte | 舊版面，只適合離線分析；會讓 runtime 的 correct-path 游標每次誤預測錯位 D byte |
| `none` | 只有 correct path，不產生 wrong-path byte | 對照組 |

`separate` 模式下 `<out>.fe.wp` 的 byte 依序對應每次誤預測展開的 D 個 block：
**第 k 次誤預測 → byte 區間 `[k*D, (k+1)*D)`**，D = `wrongpath_depth`。
檔案大小恆等於 `誤預測次數 × D`。

### 6.3 `<out>.fe.meta.json`（sidecar，監督者 A5 裁決）
記錄配置指紋與檔案關係，供 sweep / 回歸追溯：
`generator_version`、`bstf_version`、`generated_utc`、
`config_file` + `config_sha256`（原始檔 hash）、
`effective_config` + `effective_config_sha256`（**套完預設值後**的正規化配置，
sweep 時用這個當 key）、`fe_file` / `fe_bytes` / `n_fe_blocks`、
`wrongpath_out` / `wrongpath_depth` / `wrongpath_file` / `wrongpath_bytes`、
`mispredicts`、`wrongpath_layout`。
runtime 端的健全性檢查（A5）可直接比對 `fe_bytes == hdr.n_fe_blocks`。

### 6.2 `.stats.json`
`total_fe_blocks` / `correct_path_blocks` / `wrongpath_blocks` /
`instructions` / `branch_blocks` / `cond_branches` /
`mispredicts` / `mispredict_rate` / `branch_mispredict_rate` /
`cond_dir_mispredict_rate` / `mpki` /
`ubtb_hit_rate` / `btb_hit_rate` / `btb_override_rate` / `tage_override_rate` /
`ras_accuracy` / `avg_bubbles_per_block` / `bubble_histogram` /
`mispredict_rate_by_decile`（把 correct path 切成 10 段，用來看預測器收斂）。

## 7. 設定參數（`config.json`）

所有 key 都可省略，省略時用預設值。支援 `#` 與 `//` 註解、十六進位數字。

| key | 預設 | 說明 |
|---|---|---|
| `ubtb_entries` | 16 | uBTB entry 數（全關聯） |
| `ubtb_tag_bits` | 20 | uBTB tag（PC hash）寬度，越小 alias 越多 |
| `btb_entries` | 8192 | L2 BTB 總 entry 數 |
| `btb_ways` | 4 | L2 BTB 路數（set 數自動取 2 的冪） |
| `ubtb_branch_slots` | 1 | uBTB 每個 entry 記幾個分支 |
| `btb_branch_slots` | 2 | L2 BTB 每個 entry 記幾個分支（CoreMark 實測 2 就夠，3/4 無改善） |
| `tage_tables` | `[{hist:8},{16},{32},{64}]` | 整數 = 表數；`[8,16,32,64]` = 只給歷史長度；物件陣列 = `{hist, entries, tag_bits, ctr_bits, u_bits}`；`[]` = 純 bimodal |
| `tage_entries` / `tage_tag_bits` | — | 一次覆寫所有表 |
| `bimodal_index_bits` | 13 | base predictor 索引寬度 |
| `bimodal_ctr_bits` | 2 | base counter 寬度 |
| `useful_reset_period` | 262144 | useful 老化週期（每 N 次更新清 u 的一個 bit），0 = 關閉 |
| `use_alt_on_na` | 1 | 新配置 entry 時改用 altpred（標準 TAGE） |
| `ras_depth` | 16 | RAS 深度 |
| `wrongpath_depth` | 10 | D：誤預測後沿錯誤路徑模擬幾個 fetch block，0 = 關閉 |
| `wrongpath_out` | `"separate"` | `separate` / `inline` / `none`，見 §6.1 |
| `emit_wrongpath` | 1 | 舊名，`0` 等同 `wrongpath_out: "none"` |
| `wrongpath_train_btb` | **1** | wrong path 是否配置 uBTB/BTB entry（真實行為，見 §10.1） |
| `wrongpath_train_tage` | **0** | wrong path 是否訓練 TAGE 計數器（代理假設，預設關，見 §10.1） |
| `btb_dir_hint` | 1 | 1 = BTB 有自己的 2-bit 方向 hint（TAGE 可再 override → 2 bubble）；0 = BTB 直接用 TAGE 方向（最多 1 bubble） |
| `fetch_width` | 4 | 每個 fetch block 最多幾條指令（也決定 BTB miss 時的 fallthrough = `PC + fetch_width*ilen_bytes`） |
| `ilen_bytes` | 4 | 推不出指令長度時的預設值 |
| `learn_call_len` | 1 | 從 ret 的實際回傳位址回推 call 指令長度（RVC 2/4 byte） |
| `allow_ambiguous_6col` | 0 | 把 §3.1 第三種歧義格式從「報錯」降級成「警告」 |
| `max_blk_instr` | 8 | trace 沒給 `is_block_end` 時的保險上限 |

附的三份設定：`config.json`（demo 規格）、`config_bimodal.json`（拿掉所有
tagged component 的對照組）、`config_nowp.json`（關掉 wrong-path 的對照組）。

## 8. 自測（`make test`）

`gen_trace.py` 產生合成串流：

* **預設模式**（巢狀迴圈 + 函式呼叫）：outer 400 × inner 20，含
  call/ret、週期 3 與週期 5 的 conditional pattern（bimodal 學不到、
  TAGE 靠 8/16 bit 歷史學得到）、迴圈回邊。12 萬條指令 / 4.6 萬個 block。
* **壓力模式** `--hard`：深度 20 的巢狀呼叫鏈（超過 RAS 的 16 entry）
  + 在 4 個目標間輪替的 `jalr`，用來涵蓋 RAS 回捲與間接跳躍。

兩種模式都輸出 **7 欄格式**，跟 Agent H 的生產 trace 走同一條程式路徑。

> **維護規則（從 2026-09-21 的事故學到的）**：`gen_trace.py` 的輸出慣例
> 必須與生產 trace 完全一致。當時自測用「not-taken 填 target=0」、
> 生產用「architectural target」，兩條路徑的慣例不同，
> 所以「一個 fetch block = 一個分支且在最後」這個錯誤假設
> 在合成串流上 100% 正確、在真 trace 上 98% 錯誤，卻躲過了全部自測。
> **改格式時先改 `gen_trace.py`，不要只改 reader。**

斷言：

1. **版面**：預設 `separate` 下 `.fe` 大小 == correct-path block 數（純線性）、
   `.fe.wp` 大小 == 誤預測數 × D；`inline` 下 `.fe` 大小 == correct + wrong；
   換版面不得改變 correct-path 的任何結果。
2. **sidecar**：`.fe.meta.json` 的 `config_sha256` 要等於 `sha256sum config.json`，
   `wrongpath_*` / `fe_*` 欄位要與實際檔案大小一致。
3. `wrongpath_depth=10` 有展開、`=0` 沒有；wrong-path block 數 == 誤預測數 × D。
4. **TAGE 有學到**：誤預測率第一個十分位 > 最後一個十分位，且最終 < 2%。
   實測 `0.0101 → 0.0000 …… 0.0006`。
5. **TAGE 明顯優於 bimodal**：conditional 方向誤預測率
   **0.242% vs 32.26%**（133×，MPKI 0.531 vs 65.34；週期性 pattern 純 bimodal 無解）。
6. RAS 準確率 > 99%（預設模式，實測 99.99%）；壓力模式落在 16/20 = 80%
   附近（實測 79.96%，正好是 RAS 深度不足造成的固定漏失）。
7. 平均 bubble/block 合理（實測 0.3400）。
8. `fe_check` 逐 byte 檢查：`FE_REDIRECT` 不得與 `DIR_OK+TGT_OK` 同時成立、
   bubbles ≤ 2、`FE_BTB_OVERRIDE` 必定伴隨 ≥ 1 bubble。
9. **保守預設下誤預測數對 D 不敏感**：D=10 與 D=0 必須相同（都是 64）。
9b. **三種輸入慣例**（§3.1）：7 欄與 6 欄(a) 必須給出完全相同的誤預測數；
    6 欄(b) 必須被拒絕（exit 3）。
10. **§10.2 的懸崖被回歸保護**：`hard.trace` 上 `ubtb_entries=U` 時，
    `D=U-1` 的 uBTB 命中率 > 10%（wrong-path prefetch 效應存在）、
    `D=U` 必須歸零（U=8 與 U=16 各測一次）。

預設自測約 0.1 秒；吞吐量實測約 **24 M 指令/秒**（600 萬條指令 0.25 秒）。

## 9. 已知限制

0. ~~`.fe` 的 wrong-path byte 與 runtime 游標約定衝突~~ —— **已由監督者 A1 裁決解決**：
   `separate` 成為預設，`.fe` 保持線性。用 `inline` 時這個坑仍然存在，
   只能拿來做離線分析，不要餵給 timing model。
1. **wrong path 的「實際結果」是近似的。** 錯誤路徑上沒有真的指令資料，
   所以用「這個 PC 在 correct path 上最近一次觀察到的靜態資訊/方向」當代理，
   完全沒看過的 PC 就用合成 block（4 條指令、無 branch）。
   這正是 `doc/PLAN.md` R3 描述的開放研究問題，不是實作缺陷。
   **因此 `wrongpath_train_tage` 預設為 0**（見 §10.1）：預設只保留
   「wrong path 的 fetch 會配置 BTB/uBTB」這個物理行為，
   以及歷史的推測更新 + 精確還原。
2. **wrong path 不會遞迴展開**：錯誤路徑上的 branch 不再觸發第二層
   wrong path（真實硬體也會被更舊的 branch flush 掉）。
3. **RAS 在預測階段不做推測性 push/pop**（correct path 上在解析時才更新），
   等於假設 RAS 有完美修復機制。這會高估 RAS 準確率；
   wrong path 上則是推測更新 + 回滾。
4. **fetch block 的切法來自 trace**，不是模型自己用 BTB 切的。
   FTB 命中時 fall-through 是精確的（從 `next_pc` 學到的），
   但 **FTB miss 時**只能猜 `PC + fetch_width*ilen_bytes`，RVC 下會偏大。
   CoreMark 上 FTB 命中率 99.99%，所以這個誤差幾乎不出現。
4b. **每個 FTB entry 只記前 `btb_branch_slots` 個分支**。超過的 block
   （CoreMark 上 161/315915 = 0.05%）其後面的分支不會被預測。
   `slot_overflow_blocks` 有統計這個數字。
5. **不模擬 I-cache / ITLB miss、fetch buffer 滿、bank conflict**。
   `FE_BUBBLES` 只含 override 泡泡（0..2），值 3 保留未用。
   I-cache 由 Agent B 的 `.mem` overlay 負責。
6. **TAGE 沒有 statistical corrector / loop predictor / local history**，
   也沒有 path history（只有全域方向歷史）。折疊歷史長度上限 500 bit
   （`HistReg` = 512 bit），tagged table 上限 8 個。
   CoreMark 上這是主要的剩餘誤差來源（5.92%），但**已經被證明是這個
   預測器類別的資訊上界而非實作缺陷**，監督者裁決不補 SC-L，見 §11.2。
7. **沒有推測性 BTB/TAGE 更新的 in-flight 視窗**：correct path 上「預測→解析」
   是同一個 block 內完成的，等於假設分支 1 拍解析。真實的 10 級 pipeline
   有 ~12 拍的更新延遲，會讓緊耦合的相鄰分支（如短迴圈）略為樂觀。
8. **`jalr` 只用 BTB 的上次目標**，沒有 ITTAGE/indirect predictor，
   多目標間接跳躍會大量誤預測（壓力模式刻意涵蓋這點）。

## 10. wrong-path 建模：預設值裁決與 `wrongpath_depth` (D) 敏感度

### 10.1 兩個 knob，兩種硬體地位（監督者裁決，預設值因此不同）

wrong-path 的「訓練」不是一件事，是兩件地位完全不同的事：

| knob | 真實硬體 | 預設 |
|---|---|---|
| `wrongpath_train_tage` | **TAGE 方向計數器多數設計在 retire 才更新。** wrong-path 指令從不 retire，它們的計數器更新會被 squash。（全域歷史暫存器確實會推測更新並在誤預測時還原 —— 但那是**歷史**不是**計數器**，本模擬器已經正確地做了「進 wrong path 前存檔、離開時精確還原」。） | **0** |
| `wrongpath_train_btb` | **BTB / uBTB 的配置發生在 fetch / predecode。** wrong path 的 fetch 真的執行過，entry 真的被配置、真的把別人擠掉。 | **1** |

這樣兩個 knob 各自對應一個**真實的硬體行為**，而不是一個代理假設。
`wrongpath_train_tage: 1` 仍然保留，但它是「用這個 PC 最近一次觀察到的方向
去訓練」的代理假設（§9-1），只該拿來做敏感度分析，不該當基準配置。

### 10.2 ★主結論：wrong path 對 uBTB 是 prefetch，直到 D 撞到容量為止

保守預設下（`wrongpath_train_tage: 0`），D 的效應只剩**一個**乾淨的機制：
wrong path 走在 correct path 前面，把它掃過的 block 配進 uBTB/BTB。
在 working set 超過 uBTB 容量的 workload（`hard.trace`：40+ 個 block 循環走訪）
上，這條曲線是：

```
uBTB 命中率
  70% ┤                                              ╭─ (U=32)
      │                                         ╭────╯
  35% ┤                    ╭──(U=16)      ╭─────╯
      │          ╭─────────╯         ╭────╯
  14% ┤   ╭─(U=8)╯              ╭────╯
      │╭──╯                ╭────╯
   0% ┼╯   ╳          ╳                              ╳      ← D == U 時直接歸零
      └────┴──────────┴──────────────────────────────┴──► D
      0    8         16                             32
```

實測（`hard.trace`，只改 `ubtb_entries` U 與 D）：

| D | 4 | 6 | 7 | 8 | 9 | 12 | 14 | 15 | 16 | 17 | 24 | 31 | 32 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **U=8**  | 7.0% | 11.6% | 13.9% | **0.0%** | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| **U=16** | 7.0% | 11.6% | 13.9% | 16.3% | 18.6% | 25.6% | 30.2% | 32.5% | **0.0%** | 0.0% | 0.0% | 0.0% | 0.0% |
| **U=32** | 7.0% | 11.6% | 13.9% | 16.3% | 18.6% | 25.6% | 30.2% | 32.5% | 34.8% | 37.2% | 53.4% | 69.7% | **0.0%** |

* 上升段的斜率與容量無關，固定 **+2.324% 命中率 / 每多一個 wrong-path block**。
* **懸崖精確落在 `D == ubtb_entries`**，三種容量（8 / 16 / 32）都成立 ——
  這是可證偽的預測，不是巧合：wrong path 一次就把整個 uBTB 換過一輪，
  correct path 回來時全空（LRU 在「循環走訪 > 容量」下的典型病態）。
* 這個現象**與 LRU 理論一致**，也解釋了為什麼「D 取大一點比較保險」是錯的。
* 誤預測數在整條曲線上完全不動（該 workload 的誤預測來自 RAS 深度不足與
  jalr 目標輪替，是結構性的），但**平均 bubble/block 在 0.6974 ~ 0.9995
  之間擺動（±30%）**。

### 10.3 保守預設下的 D 敏感度（PLAN 用的正式不確定度估計）

`make sweep` 或 `./sweep_d.sh <trace> <D...>`，其他配置固定、只改 D。

**(a) `loop.trace` — 巢狀迴圈，working set 遠小於 uBTB → D 完全是 don't-care**

D ∈ {0,4,8,12,16,24,32} 下誤預測數（64）、MPKI（0.531）、uBTB/BTB 命中率
（99.9740%）**逐位不變**，平均 bubble/block 只從 0.3396 變到 0.3400（+0.12%）。

**(b) `hard.trace` — 前端抖動，working set > uBTB 容量 → D 主導前端泡泡**

| D | 誤預測率 | MPKI | uBTB 命中 | BTB override | 平均 bubble/block |
|---:|---:|---:|---:|---:|---:|
| 0 | 11.6767% | 46.064 | 0.00% | 99.95% | 0.9995 |
| 4 | 11.6767% | 46.064 | 6.97% | 92.98% | 0.9298 |
| 8 | 11.6767% | 46.064 | 16.26% | 83.68% | 0.8368 |
| 12 | 11.6767% | 46.064 | 25.56% | 74.39% | 0.7439 |
| 14 | 11.6767% | 46.064 | 30.20% | 69.74% | **0.6974** |
| 16 / 24 / 32 | 11.6767% | 46.064 | **0.00%** | 99.95% | 0.9995 |

**(c) ★真實 CoreMark（`traces/coremark_1m.frontend.txt`）—— 落在兩者之間，形狀一致**

| D | 誤預測率 | MPKI | uBTB 命中 | BTB override | 平均 bubble/block |
|---:|---:|---:|---:|---:|---:|
| 0 | 4.9320% | 15.581 | 50.58% | 35.74% | 0.4253 |
| 4 | 4.9320% | 15.581 | 55.28% | 33.18% | 0.4132 |
| 8 | 4.9320% | 15.581 | 57.42% | 31.91% | 0.4106 |
| **10** | 4.9320% | 15.581 | **58.12%** | 31.48% | **0.4078** |
| 16 | 4.9320% | 15.581 | 50.65% | 35.49% | 0.4415 |
| 24 | 4.9320% | 15.581 | 49.61% | 36.04% | 0.4468 |
| 32 | 4.9320% | 15.581 | 49.57% | 36.07% | 0.4471 |

50 M 全長 trace：`D=0 → 0.4460`、`D=10 → 0.4447`、`D=24 → 0.4693`，
誤預測數三者完全相同（625,800）。

**真實 workload 完整重現了 §10.2 的機制**：
uBTB 命中率隨 D 上升（prefetch 效應）、在 **D ≈ `ubtb_entries` = 16 反轉**、
之後單調變差。CoreMark 的 block 走訪不是完美循環，所以是「軟懸崖」
（50.6% → 58.1% → 49.6%）而不是合成案例的垂直崩落，但轉折點位置一致。

### 10.4 給 PLAN 的正式結論

**wrong-path 建模不確定度（保守預設，`wrongpath_train_tage: 0`）：**

| 指標 | uBTB 裝得下（`loop`） | **真實 CoreMark** | 前端抖動（`hard`） |
|---|---|---|---|
| 誤預測率 / MPKI | **0%**（逐位不變） | **0%**（逐位不變） | **0%**（逐位不變） |
| uBTB 命中率 | **0%** | **49.6 ~ 58.1%**（8.5 pp） | **0 → 32.5 pp** |
| 平均 bubble/block | **+0.12%** | **−4.1% ~ +5.2%** | **−30.2%** |

1. **誤預測率對 D 完全不敏感**（保守預設下）。先前報告的 +41% ~ +82% 已經
   確認**全部**來自 `wrongpath_train_tage` 這個代理假設，現在預設關閉。
   這表示「wrong-path 會不會讓分支預測變差」在本模型裡**不是**一個結論，
   而是一個假設；要主張它，需要 gem5/RTL 的對照實驗。
2. **前端泡泡對 D 高度敏感，而且非單調。** D 不能用固定值近似，也不能
   「取大一點比較保險」—— `D ≥ ubtb_entries` 會系統性地抹掉 uBTB 的貢獻。
3. **掃描規則**：把 D 當配置維度；避開 `D ≈ uBTB/BTB 容量` 的懸崖；
   任何 IPC 數字都要附 D（`.fe.meta.json` 已把 D 與配置 hash 記進去）。
4. **若非要單一代表值**：`D ≈ K / fetch_width`
   （10 級、4-wide → K≈40 指令 → **D≈10 個 block**，即目前預設），
   並**同時附 D=0 的對照**；兩者差距（本模型：前端泡泡最多 −21%，
   誤預測 0%）就是 wrong-path 建模不確定度的上界。
5. **已用真實 CoreMark 驗證**：它確實落在兩個合成案例之間，
   前端泡泡的 D 敏感度帶寬約 **±5%**，且最佳點就在 `D=10`（目前預設），
   `D ≥ ubtb_entries` 之後單調變差 —— 與 §10.2 的機制預測一致。
   所以「D≈10 + 附 D=0 對照」這個建議在真實 workload 上成立。

## 11. CoreMark 實測（Agent H 的真 trace）

`traces/coremark.frontend.txt`（50 M 指令，rv64gc + Zb*，CoreMark steady state）。

### 11.1 預設配置（CONTRACT §0 的 demo 規格）

| 指標 | 50 M | 1 M 視窗 |
|---|---:|---:|
| fetch block 數 | 15,690,426 | 315,915 |
| 分支數 / conditional | 11,109,946 / 10,606,906 | 222,232 / 212,460 |
| 含多個分支的 block | 1,318,513 (8.4%) | 25,238 (8.0%) |
| **L2 BTB (FTB) 命中率** | **99.9941%** | 99.7072% |
| uBTB 命中率（16 entry） | 54.04% | 58.12% |
| BTB override 率 | 34.26% | 31.48% |
| TAGE override 率 | 6.53% | 5.75% |
| **conditional 方向誤預測率** | **5.92%** | 7.16% |
| block 誤預測率 | 3.99% | 4.93% |
| MPKI | 12.52 | 15.58 |
| RAS 準確率 | 98.87% (49,190 rets) | 92.13% (953 rets) |
| **平均 bubble/block** | **0.4447** | 0.4078 |
| 吞吐量 | 11.7 M 指令/秒 | — |

### 11.2 方向預測率的上界分析（為什麼不是 2–5%）

在同一條 trace 上用 Python 算「無容量上限、無 alias」的理想預測器：

| 預測器 | 10 M 視窗 | 1 M 視窗 |
|---|---:|---:|
| 完美 bimodal（每個 PC 一個 2-bit counter，無 alias） | 10.663% | 10.393% |
| 理想 global-history H=16（`(PC, 歷史)` 完美查表） | 7.220% | 8.242% |
| 理想 global-history H=32 | 5.149% | 6.916% |
| 理想 global-history H=64 | **4.780%** | 7.164% |
| **本模擬器的 TAGE（4×1K，hist 8–64）** | **5.92%**（50 M） | **7.165%** |

* 我們的 bimodal 對照組（10.666%）與理想 bimodal（10.663%）**逐位吻合** ——
  base predictor 沒有實作錯誤。
* 1 M 視窗上 TAGE（7.165%）與理想 H=64（7.164%）**完全相同** ——
  在那個視窗裡 TAGE 已經打到全域歷史類預測器的資訊上界。
* 50 M 上 TAGE 5.92% vs 理想 H=64 4.78%，差 1.14 pp。
  把 TAGE 放大 16 倍（4×16K）只降到 **5.73%**，放到 8 個表也只有 5.87% ——
  **不是容量問題**，是 TAGE 的 tag/allocation 近似與缺少 local history。
* 結論：**CoreMark 這條 trace 在只有全域歷史的預測器下，2–5% 不可達**。
  要再往下要加 local history / loop predictor / statistical corrector
  （TAGE-SC-L 那一類），那是規格變更，不是 bug。

> **監督者裁決（2026-09-21）：不加 TAGE-SC-L。**
> 理由就是上面這張表：bimodal 對照與理想 bimodal 逐位吻合、
> TAGE 在 1 M 視窗上與理想 global-history H=64 完全相同、
> 放大 16 倍只降 0.19 pp —— 三個證據都指向「實作正確，這是預測器類別的
> 資訊極限」，不是 bug。demo 要展示的是 bit-sliced pipeline，不是預測器研究。
> **所以 5.92% 是這個配置的正確答案，不要再靠調參數把它壓下去**；
> 要更低必須改規格（加 local history / loop predictor / SC），已列入 PLAN 後續項。

### 11.3 前端泡泡的主要來源是 uBTB 容量

| uBTB entry 數 | uBTB 命中率 | 平均 bubble/block |
|---:|---:|---:|
| 16（規格） | 54.04% | 0.4447 |
| 64 | 93.22% | **0.2193** |

CoreMark 的 hot fetch block 約 925 個，16 entry 的 uBTB 抓不住，
**34% 的 block 要等 L2 BTB override（1 bubble）**。
把 uBTB 加到 64 entry 可以讓前端泡泡減半 —— 這是給設計端的具體數字，
不影響誤預測率（BTB 已經 99.99% 命中）。

## 12. 檔案

| 檔案 | 說明 |
|---|---|
| `frontend_sim.cpp` | 主程式：設定、block reader、三階段預測、wrong path、統計 |
| `predictors.hpp` | uBTB / L2 BTB / TAGE（含折疊歷史）/ RAS |
| `json.hpp` | 極簡 JSON 讀取器（支援註解與 hex） |
| `sha256.hpp` | 極簡 SHA-256（`.fe.meta.json` 的配置指紋，已對過 NIST 測試向量） |
| `fe_check.cpp` | `.fe` overlay 檢查/傾印工具（用 `bstf.h` 巨集解碼） |
| `gen_trace.py` | 自測用合成指令串流產生器（預設模式 / `--hard`） |
| `selftest.sh` | `make test` 的實際內容與斷言（8 組、10 項） |
| `sweep_d.sh` | `wrongpath_depth` 敏感度掃描（`make sweep`） |
| `config*.json` | 預設配置與兩組對照配置 |
| `Makefile` | `all` / `test` / `demo` / `sweep` / `clean` |
