# bstm-cc —— Yosys word-level 網表 → bit-sliced C

BSTM 編譯鏈的後半段（`doc/PLAN.md` 第 6 章）。吃 Yosys `write_json` 的
word-level 網表，吐出「每一行同時算 64/128/512 個 instance」的純 bitwise C。

`prototype/gen.py`（167 行）的產品化版本。相對於原型多了：

| 項目 | 原型 | bstm-cc |
|---|---|---|
| liveness + slot 配置 | 無（每個 net 一個區域變數） | 有（PLAN §6.4a） |
| 常數傳播 / 代數化簡 / CSE | 無 | 有（PLAN §6.4b） |
| cell 覆蓋 | 12 種 | 34 種組合 + 7 種 FF + memory |
| 不支援的 cell | 靜默產生 `/* TODO */` | 明確報錯 + cell 名 + 來源行號 |
| 多 backend | 無 | `c64` / `neon` / `avx512`（PLAN §6.4c） |
| `SRST_VALUE` / polarity | 忽略（一律當 0、一律 active-high） | 完整處理 |
| `$pmux` 的 default 路徑 | 忽略 A | 照 yosys simlib 語意 |
| 驗證 | 無 | Verilator 逐 cycle 對拍 + 30 個自測 |

---

## 用法

```
bstm-cc <netlist.json> <top-module> [--backend c64] [--lanes 64] [-o out.c] [--stats]
```

| 選項 | 說明 |
|---|---|
| `--backend {c64,neon,avx512}` | word 型別。`c64`=`uint64_t`(64 lane)、`neon`=`uint32x4_t`(128 lane)、`avx512`=`__m512i`(512 lane) |
| `--lanes N` | 只做檢查用；必須等於 backend 的天然 lane 數，不然報錯 |
| `-o FILE` | 輸出（預設 stdout） |
| `--stats` | 把 net / slot / op 統計印到 stderr |
| `--no-comments` | 不放 net 名稱註解（大設計可少掉一半檔案大小） |
| `--no-alloc` | 關掉 slot 配置，每個值一個 slot。**只用來做效能對照** |
| `-q` | 不印警告 |

`<top-module>` 在網表只有一個 module（或有 `top` 屬性）時可以省略。

### 建議的 yosys 腳本

```bash
docker run --rm -v "$PWD":/w -w /w hdlc/yosys:latest yosys -p '
  read_verilog stage.v chain.v
  hierarchy -check -top chain
  proc; opt_expr; opt_clean; flatten; opt -fast
  memory_dff; opt_clean          # 有記憶體才需要，見下面「記憶體」
  write_json chain.json; stat'

./bstm-cc chain.json chain --stats -o chain_bs.c
gcc -O3 -march=native -c chain_bs.c
```

**不要**跑 `techmap` / `abc` / `memory_map`。bstm-cc 要的是停在 word level 的網表
（PLAN §6.2）；打成 gate-level 會讓 cell 數爆炸好幾個數量級。

---

## 產生出來的 C

```c
typedef uint64_t vec_t;                       /* backend 決定 */
typedef struct { vec_t q[BSTM_NSTATE_ARR]; } state_t;

void bstm_init_state(state_t *st);            /* 清零 + $meminit */

void eval_cycle(state_t *restrict next, const state_t *restrict cur,
                /* 輸入埠，模組宣告順序，clk 已移除 */
                vec_t rst, vec_t in_valid, const vec_t in_tag[6], ...,
                /* 輸出埠，模組宣告順序 */
                vec_t *in_ready, vec_t *out_valid, vec_t out_tag[6]);

const bstm_model_desc_t *bstm_describe(void); /* PLAN §13.2 的簡化版 */
```

規則：

* 1-bit 埠 → `vec_t`（輸出是 `vec_t *`）；多 bit 埠 → `vec_t name[W]`，
  index 0 是 LSB（`upto` 的 wire 會被正規化）。
* 時脈埠（只接到 FF/memory `CLK` 的 1-bit 輸入）不會出現在參數列 ——
  cycle-based 模型不需要它。
* **`next` 和 `cur` 不可以指到同一塊記憶體。** state 更新是一算好就寫出去的
  （這正是 slot 數壓得下來的原因），如果 alias 會讀到新值。
* 輸出埠全部在函式最後才寫，所以輸入/輸出陣列指到同一塊是安全的。
* 純 2-state。Verilator 對拍時要加 `--x-assign 0 --x-initial 0`（PLAN §6.4e）。

---

## liveness + slot 配置（P0，這支工具的重點）

原型把每個 net 宣告成一個 `vec_t` 區域變數，而且把所有 FF 更新擠在函式尾巴，
所以整個設計的中間值同時活著。兩萬 cell 的模型會因此 spill 到 ~10 MB 工作集，
掉出 L2（PLAN §10.2）。

bstm-cc 的做法：

1. **DCE** —— 從 sink（state 更新、輸出埠）倒推，砍掉沒用到的 op。
2. **排程** —— 對 IR 的 op DAG 做暫存器壓力導向的貪婪 list scheduling。
   代價函數是「這個節點新增幾個活值 − 殺掉幾個活值」。
   **state 更新本身也是可排程的節點**，所以 D 值一算好馬上寫進 `next->q[]`，
   不會一路活到函式尾巴。這一項是壓縮比從 4.5x 跳到 63x 的主因。
3. **配置** —— 直線程式的干涉圖是 interval graph，依起點做線性掃描貪婪著色
   就是最佳解，不必跑一般的圖著色。

實測：

| 設計 | cell | net bit | slot | 壓縮比 | 工作集 (64 lane) |
|---|---|---|---|---|---|
| `ops`（cell 覆蓋測試） | 68 | 310 | 14 | 22x | 112 B |
| `chain`（32 級） | 815 | 1,774 | 28 | **63x** | 224 B |
| `chain_big`（800 級） | 20,015 | 43,246 | 39 | **1,109x** | **312 B** |

20,015 cell 的模型只要 312 bytes 的工作集 —— 完全在 L1 裡，達成 PLAN §10.2
「數百個 slot、塞進 L2」的目標還有很大餘裕。

`--stats` 會把這張表印出來。

---

## 支援的 cell

**組合**（34 種）

```
$and $or $xor $xnor $not $pos $neg $buf
$reduce_and $reduce_or $reduce_xor $reduce_xnor $reduce_bool
$logic_and $logic_or $logic_not
$eq $ne $eqx $nex $lt $le $gt $ge          （signed / unsigned 都支援）
$add $sub
$mux $pmux $bmux $demux                    （$mux 的多位元 select 會當 $bmux 處理）
$shl $shr $sshl $sshr                      （log-shifter，位移量超出寬度會正確移光）
```

**時序**

```
$dff $dffe $adff $adffe $sdff $sdffe $sdffce
$memrd $memrd_v2 $memwr $memwr_v2 $mem $mem_v2 $meminit $meminit_v2
```

`CLK_POLARITY` / `EN_POLARITY` / `SRST_POLARITY` / `ARST_POLARITY` /
`SRST_VALUE` / `ARST_VALUE` 都有處理。`$sdffe`（reset 優先）與 `$sdffce`
（enable 優先）的優先權差異也有分開實作。

**記憶體**

同步讀 + 同步寫，展成暫存器陣列 + 位址比較器：

* 寫：`we_e = EN[b] & (ADDR == e)`，`mem[e][b] <= we_e ? DATA[b] : mem[e][b]`
* 讀：`rdata[b] <= OR_e( mem[e][b] & (ADDR == e) )`，讀的是**寫入前**的內容
  （read-first / non-transparent）
* 多個寫埠依 `PORTID` 由小到大套用，大的蓋掉小的

成本是 `SIZE × WIDTH × (寫埠數+1)` 個 op，所以只適合小表格。大表格請照
PLAN §6.5 改成 bit-matrix 或走 refill 區。

---

## 不支援（會明確報錯，附 cell 名與來源行號）

| cell / 情況 | 原因與改寫建議 |
|---|---|
| `$mul` `$div` `$mod` `$divfloor` `$modfloor` `$pow` | 違反建模規則 1（欄位 ≤ 8 bit、禁止寬算術）。改成查表或移位加法 |
| `$dlatch` `$adlatch` `$dlatchsr` `$sr` | latch 違反規則 3。改成 `always @(posedge clk)` |
| `$dffsr` `$dffsre` `$aldff` `$aldffe` | 同時有非同步 set/reset 或非同步 load。改成同步 |
| **非同步讀的記憶體**（`$memrd` `CLK_ENABLE=0`） | 違反規則 3。改成 `always @(posedge clk) rdata <= mem[a];`，並在 yosys 腳本加 `memory_dff` 把那顆暫存器併進讀埠 |
| 非同步寫的記憶體 | 同上 |
| transparent（write-first）讀埠 | 改成 read-first，或在 RTL 自己做 bypass |
| negedge 的 FF / 記憶體 | 規則 3：純同步單一 clock domain |
| `$assert` `$assume` `$cover` `$live` `$fair` `$anyconst` `$anyseq` | 加 `chformal -remove` |
| `$print` `$check` | 違反規則 5（禁止 `$display`） |
| `$specify2` `$specify3` `$specrule` | 用 `read_verilog -nospecify` |
| 非 `$` 開頭的 cell | 網表沒 flatten，或跑過 techmap/abc |
| `inout` 埠 | 不支援 |
| 組合迴圈 | 會報「還有 N 個 cell 排不進去」並列出前幾個 |
| 埠名撞到產生的 C 保留字 | 會報錯要求改名 |

### 已知限制（不報錯但要知道）

1. **`$adff` / `$adffe` 當成同步 reset 處理**（會發警告）。cycle-based 模型沒有
   「cycle 中途」的概念。如果 reset 只在 clock edge 對齊時變化，結果一樣；
   否則跟 Verilator 會差一拍。照 CONTRACT 規則 3 建模就不會踩到。
2. **加法器是 ripple carry**（PLAN §6.4d）。8-bit 以上欄位的進位鏈會殺 ILP，
   但依規則 1 模型裡不該有寬欄位，所以沒做 carry-lookahead。
   進位用 `V_MAJ3`，在 AVX-512 上是一條 `vpternlogd`。
3. **多時脈域不支援**，所有 FF 必須是同一個 posedge。
4. `--backend neon` / `avx512` 只保證產得出程式碼、語法正確（avx512 有做
   `gcc -mavx512f -fsyntax-only` 的自測）；逐 cycle 對拍只在 `c64` 上做過。
5. `$pmux` 依 yosys simlib 語意做成「index 大的優先」的串接 mux，深度是 `S_WIDTH`。
   CONTRACT 規則 4 已經要求優先用 if/else 鏈，所以實務上很少見。

---

## 測試

```bash
make test     # 30 個自測（pytest 或 unittest 都可以）
make cosim    # 跟 Verilator 逐 cycle 對拍 ops / chain / memtest
make bench    # chain 的效能對照：原型 vs 無 liveness vs 有 liveness
```

`make test` 需要 `gcc`；`make cosim` 另外需要 `verilator` 和 docker 的
`hdlc/yosys:latest`。缺工具的測試會自動 skip。

測試內容：

* IR 化簡的代數恆等式、CSE、De Morgan
* slot 配置：壓縮比門檻、slot 密集、**同 slot 的生命期不重疊**
* 錯誤訊息含 cell 名與來源行號
* 三個 backend 的 codegen；avx512 的語法檢查
* 合成網表上的 cell 語意：`$bmux` `$demux` `$xnor` `$reduce_xnor` `$sshl`
  `$pos` `$adff`、位移溢位、有號/無號比較的全 16×16 真值表
* `chain` 的 checksum 必須等於原型 `gen.py` 的 `aa38064b48fd97c0`
* Verilator 對拍：`ops`（34 種算子）、`chain`（815 cell）、`memtest`（記憶體）

### 檔案

```
bstm-cc              進入點
bstm_cc/netlist.py   JSON 解析、參數解碼
bstm_cc/ir.py        bit-sliced IR + 化簡過的建構子（常數傳播 / CSE / De Morgan）
bstm_cc/cells.py     cell dispatch table（PLAN §6.3）
bstm_cc/compiler.py  state 配置、拓樸排序、cell 展開、DCE、排程、liveness、slot 配置
bstm_cc/backend.py   word 型別抽象（c64/neon/avx512）+ C 產生
bstm_cc/errors.py    錯誤型別
tests/               自測、Verilator 對拍、測試用 Verilog、規模測試產生器
```
