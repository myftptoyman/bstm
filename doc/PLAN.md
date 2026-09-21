# BSTM 計劃書
## Bit-Sliced Timing Model —— 以 Verilog 為建模語言、ISS 為功能後端的高吞吐微架構效能探索平台

| | |
|---|---|
| 文件版本 | v1.0 |
| 日期 | 2026-09-21 |
| 專案代號 | BSTM（Bit-Sliced Timing Model，暫定名） |
| 前身 | `nds_spiny_sim`（2021，Verilog pipeline model + Spike） |
| 狀態 | 提案，待 Phase 0 決策閘 |

---

## 目錄

1. [背景與問題陳述](#1-背景與問題陳述)
2. [既有技術地景](#2-既有技術地景)
3. [核心洞察與價值主張](#3-核心洞察與價值主張)
4. [總體架構](#4-總體架構)
5. [模型層：A-Port 抽象與建模規範](#5-模型層a-port-抽象與建模規範)
6. [編譯鏈：Verilog → Yosys → bit-sliced backend](#6-編譯鏈verilog--yosys--bit-sliced-backend)
7. [ISS 整合：Spike 前置管線與 trace 契約](#7-iss-整合spike-前置管線與-trace-契約)
8. [執行期：instance 佈局、排程與結果收集](#8-執行期instance-佈局排程與結果收集)
9. [驗證策略](#9-驗證策略)
10. [效能模型與量化預估](#10-效能模型與量化預估)
11. [風險登記冊](#11-風險登記冊)
12. [分階段路線圖與決策閘](#12-分階段路線圖與決策閘)
13. [程式碼組織與介面定義](#13-程式碼組織與介面定義)
14. [未來延伸方向](#14-未來延伸方向)
15. [附錄](#15-附錄)

---

# 1. 背景與問題陳述

## 1.1 起點

2021 年的 `nds_spiny_sim` 專案提出了一個想法：**用 Verilog 描述 pipeline 的時序結構，把指令的實際語意交給 Spike（RISC-V ISS）處理**，藉此省略 RTL 的大量細節，在還沒有 RTL 的階段就能評估一個 CPU 微架構的效能好壞。

這個想法在學術上有正式名稱，叫做 **decoupled functional/timing simulation**（功能與時序分離的模擬）。它不是新東西，但也從來不是主流。五年後回頭看，這個方向本身是對的，當時沒做下去的原因是**速度**：Verilog 模擬器的吞吐量撐不起有意義的設計空間探索（DSE）。

本計劃書要解決的，正是那個速度問題 —— 但解法不是「讓單次模擬變快」，而是**換一個軸來量**。

## 1.2 效能模型的三難

任何微架構效能評估方法，都同時被三件事拉扯：

```
                  準確度
                  (accuracy)
                     /\
                    /  \
                   /    \
                  /      \
                 /  三難   \
                /          \
               /____________\
         速度                 可得時間點
      (speed)                (availability)
```

- **RTL 模擬 / FPGA 模擬**：準確度滿分（就是 RTL 本身），但 availability 是零 —— 要先有 RTL，而效能決策必須在 RTL 之前做完。
- **C++ 效能模型**（gem5、Sniper、業界內部模型）：availability 好，速度中等，準確度對真晶片誤差約 7–20%。但模型跟 RTL 是兩份不相干的東西，correlate 時互相對不起來，而且模型可能描述出做不出來的硬體。
- **Verilog timing model**（本專案）：availability 好、語意即硬體語意、可逐段 refine 成 RTL，但**速度是它的致命傷**。

BSTM 的整個目標，就是把第三個選項的速度問題解掉，讓「用 HDL 描述時序結構」變成一個實用的方法。

## 1.3 這次要優化的到底是什麼

必須先把目標定義清楚，否則會優化錯東西。

**我們要的是吞吐量（throughput），不是延遲（latency）。**

- 驗證工程師要的是 latency：「這個 test 跑完要多久」，因為週轉時間決定 debug 迭代速度。
- **效能探索要的是 throughput**：「一個晚上能不能掃完 200 個配置 × 30 個 SimPoint」。單次模擬快不快，不是重點。

這個區分是整個專案的基石。商用 RTL 模擬器、Verilator、GEM 全都在優化 latency；BSTM 刻意優化 throughput。兩者的最佳解在架構上是相反的。

## 1.4 成功標準

明確定義，避免事後爭論：

| 項目 | 目標 |
|---|---|
| **主要指標** | 能在一個晚上（8 小時內）掃完一個目前掃不完的設計空間 |
| 量化目標 | ≥ 10⁸ instance-cycles/s（單機，16 核） |
| 準確度目標 | 對同一份 Verilog 的 Verilator 參考模型 **bit-exact** |
| 相對準確度 | 對 RTL（未來有的時候）IPC 誤差 < 10%，且趨勢單調正確 |
| 建模生產力 | 新增一個 pipeline stage ≤ 1 人日 |
| **非目標** | 取代團隊既有的 C++ 效能模型 |
| **非目標** | 單次模擬速度贏過 Verilator |
| **非目標** | 功能正確性驗證（那是 RTL sim 的工作） |

最後兩個「非目標」特別重要。BSTM **在單一 instance 上比 Verilator 慢**（已實測，見第 10 章），這是設計上的取捨而非缺陷。如果只需要跑一次，正確答案是用 Verilator。

---

# 2. 既有技術地景

在投入之前，必須知道什麼已經有人做過、做到什麼程度、為什麼沒有普及。

## 2.1 Decoupled simulation 的四象限

Mauer、Hill、Wood（SIGMETRICS 2002）的分類至今仍是通用語彙：

```
                    誰主導時序推進
                 functional        timing
              ┌──────────────┬──────────────┐
   功能模型   │ functional-  │   timing-    │
   何時執行   │    first     │   directed   │
   ──────────  │ (trace-driven│  (Asim,      │
   先執行     │  Sniper,ZSim)│   HAsim)     │
              ├──────────────┼──────────────┤
   後執行/    │  functional- │  timing-     │
   校正       │   directed   │   first      │
              │              │ (Simics+GEMS)│
              └──────────────┴──────────────┘
```

- **functional-first**：ISS 先跑產生 trace，timing model 事後消費。無法模擬 wrong path（除非特殊處理）。速度最快、最容易平行化。
- **timing-directed**：timing model 指揮 functional model 做 fetch/execute/commit。能模擬 wrong path，精度最好，但耦合緊、難平行化。
- **timing-first**：timing model 自己粗略執行，functional model 在 retire 點校正。
- **functional-directed**：罕見。

`nds_spiny_sim` 原本落在 **timing-directed**。**BSTM 刻意退回 functional-first**，並用「預先展開 wrong-path」來補回它損失的能力。這是本專案最重要的一個架構取捨，第 7 章詳述。

## 2.2 關鍵前人工作

| 專案 | 年代 | 做了什麼 | 對 BSTM 的意義 |
|---|---|---|---|
| **Asim**（Intel/Compaq） | 1990s | timing model 是結構化 module + port 網路，ISA 行為由 feeder 提供 | 確立 port-based timing model 的形式 |
| **LSE / UNISIM** | 2000s | 專用的結構化建模語言，時序由結構決定、功能外掛 | 證明「用硬體方式描述模型」的價值 |
| **Simics + GEMS** | 2002 | timing-first 的代表作 | 四象限分類的來源 |
| **FAST**（UT Austin） | 2007 | QEMU 當 speculative functional model + FPGA 上的 Bluespec timing model，branch 解出後通知 functional model rollback | 與 `nds_spiny_sim` 架構幾乎同構 |
| **HAsim**（MIT/Intel） | 2008–2013 | timing partition 用 Bluespec 寫，functional partition 負責 decode/執行/記憶體語意；同一個 functional partition 可餵三種 target | **BSTM 最直接的思想來源** |
| **A-Port Networks** | 2008/2009 | 形式化「暴露 timing model 內在的細粒度平行性」的抽象 | **BSTM 模型層直接採用** |
| **RAMP Gold**（Berkeley） | 2010 | host multithreading：一條實體 timing pipeline 用 time-multiplexing 模擬 64 個 target core | 證明「一份邏輯 + 多份 state」的 SIMT 式做法可行 |
| **LI-BDN / Golden Gate** | 2009/2019 | latency-insensitive bounded dataflow network 的形式化，FireSim 建於其上 | 提供 cycle-exact 的形式保證 |
| **Olympia**（RISC-V 官方） | 2022– | C++ OOO 效能模型，trace-driven，吃 Spike-STF / Dromajo 產生的 STF | 現代對照組；社群公開徵求 execution-driven 版本 |
| **GEM**（NVIDIA/北大） | 2025 | Yosys → AIG → GPU 上的 virtual VLIW Boolean processor，5–40× 於 CPU 模擬器 | 證明 Yosys 前端可行；但它優化 latency 而非 throughput |
| **PPSFP / parallel-pattern** | 1985 | 把 w 個 pattern 打包進一個機器字，用 bitwise 運算一次算完 | **BSTM 執行層的核心技術** |
| **CXXRTL** | 2020 | Yosys 官方的 Verilog → C++ 模擬 backend | 證明 Yosys JSON → 自訂 backend 是成熟路徑 |

## 2.3 為什麼這個組合沒人做過

每一塊技術都成熟，沒人做的是**組合**。原因如下（重要，因為這決定了 BSTM 的定位）：

**(a) 產業不會換，理由不是技術。** Intel、AMD、Arm、Apple、Qualcomm 的效能模型全是 C++，Asim 那條線可追到 1990 年代。真正的成本不是 simulator kernel，是模型周邊：workload harness、validation suite、correlation infrastructure、視覺化工具、以及會用它的人 —— 那是幾百人年。為了速度重寫這些，沒有團隊會做。

**(b) 沒有發表誘因。** 效能模型團隊小（每家 5–20 人）、成果專有。「把效能模型加速 50 倍」對 ISCA/MICRO 不夠新，對 DAC 又太特定。學界用 gem5，因為它免費、被驗證過、可發表。

**(c) 實務上的 DSE 往往不是吞吐受限的。** 真正的效能團隊不盲掃參數空間，而是跑少數幾個精挑的配置，把時間花在**分析**。那需要可見度和互動性，不是吞吐量。而 bit-slicing 讓單一 instance 的除錯變糟。

**(d) 抽象紀律的代價高。** C++ 效能模型讓你隨手寫 `if (rob.full()) stall = true;`、`std::map`、臨時加一個 counter。bit-sliced Verilog 形式全部禁止，建模生產力大約掉 3–5 倍。

**結論**：BSTM 不應定位為「取代效能模型」，而是 **「DSE 加速器」** —— 粗掃用 BSTM 找出有意思的區域，細部分析回到可讀的 Verilator 版本。同一份 Verilog，兩個 backend，雙軌並行。

**為什麼「現在」與「RISC-V」是對的時機**：RISC-V 生態是唯一條件成立的地方 —— 沒有 legacy C++ 效能模型包袱、工具鏈開放、團隊小且多。Olympia 目前仍是 trace-driven，社群公開徵求 execution-driven 版本。

---

# 3. 核心洞察與價值主張

## 3.1 三個洞察

### 洞察一：平行度必須從 instance 維度拿，不是從電路維度

一顆 CPU 的 pipeline 沒有空間平行度。它是十幾個**結構各異**的 stage 串起來，把 fetch/decode/rename/issue/LSU 各派一條執行緒，只會得到極差的負載平衡和大量的同步。

**所以平行度要換一個維度拿。** 對效能探索而言，那個維度天然存在：

```
    平行度來源 =  (SimPoint interval)  ×  (DSE config)  ×  (target core)
                      ~30 個                ~100 個          1~N
                   ───────────────────────────────────────────────
                                  = 數千個獨立 instance
```

每個 instance 跑**完全相同的模型程式碼**，只有 state 不同。這正是 SIMD/SIMT 最理想的形狀。

### 洞察二：Verilog 的語意天生就是 branchless

`always_comb` 的語意是「所有分支都求值，最後用 mux 選」。這跟 C++ 效能模型滿是 `if`、虛擬函式、指標追逐的形態完全相反。

一個用 RTL 風格寫的 timing model，**編譯出來天然就是 predicated 的 bitwise 運算序列** —— 沒有分支、沒有間接跳躍、沒有資料相依的控制流。這是把它 bit-slice 化（乃至日後搬上 GPU）的關鍵前提，而且它是免費的，因為 Verilog 語意本來就強迫你這樣寫。

### 洞察三：bit-slicing 把 instance 維度變成免費的 SIMD

把同一條訊號在 64 個 instance 上的值，打包進一個 64-bit word：

```
     傳統（每 instance 一份）           bit-sliced（每訊號一個 word）
   ┌─────────────────────┐          ┌──────────────────────────────┐
   │ inst0: valid=1 ...  │          │ valid : [i63 ... i2 i1 i0]   │  ← 1 word
   │ inst1: valid=0 ...  │          │ stall : [i63 ... i2 i1 i0]   │  ← 1 word
   │ inst2: valid=1 ...  │   ═══►   │ tag[0]: [i63 ... i2 i1 i0]   │
   │  ...                │          │ tag[1]: [i63 ... i2 i1 i0]   │
   │ inst63:valid=1 ...  │          │  ...                         │
   └─────────────────────┘          └──────────────────────────────┘
        64 次求值                        1 次求值，64 lane 同時
```

一個 `n34 = n32 & n33;` 同時算完 64 個 instance 的 AND gate。這是 1985 年 ATPG 就在用的 PPSFP 技術，成熟、零硬體依賴、可移植。

## 3.2 實測驗證（已完成）

本計劃書的核心主張已經用可執行的原型驗證過（完整資料見第 10 章與附錄）：

| 設計規模 | Verilator（1 instance） | BSTM（每 target-cycle） | BSTM（64 lane 總計） | 淨勝 |
|---|---|---|---|---|
| 830 cell，4-bit 欄位 | 5.99 M/s | 3.24 M/s | **208 M/s** | **35×** |
| 830 cell，16-bit 欄位 | 5.91 M/s | 1.07 M/s | 69 M/s | 12× |

（i7-12700，單核，`-O3 -march=native`）

三個必須記住的事實：

1. **每 target-cycle，BSTM 比 Verilator 慢**（1.8× ~ 5.5×）。贏的全部來自 64 條 lane。
2. **淨勝 = 64 ÷ 位元序列化稅**。稅正比於平均欄位寬度。
3. **Verilator 對欄位寬度不敏感**（5.99 → 5.91），BSTM 掉 3 倍。這直接決定了建模規範（第 5 章）。

## 3.3 價值主張一句話

> 用 Verilog 描述 pipeline 時序結構、用 Spike 提供指令語意，透過 Yosys 編成 bit-sliced 程式碼，在一般 CPU 上同時模擬數千個「(SimPoint × 配置)」實例，把設計空間探索從「幾天」壓到「一小時」。

---

# 4. 總體架構

## 4.1 三層切分

BSTM 由三個時間尺度完全不同的層次組成，彼此以**檔案格式**為契約，可獨立開發與替換：

```
╔══════════════════════════════════════════════════════════════════════╗
║  離線前置層 (Offline Preprocessing)      —— 每個 workload 跑一次     ║
║  ──────────────────────────────────────────────────────────────────  ║
║   Spike ISS  ──►  BBV ──► SimPoint ──► 選出 K 個代表區間            ║
║       │                                                              ║
║       ├──► 指令 trace (correct path)                                 ║
║       ├──► wrong-path shadow (每個 branch 展開 N 條)                 ║
║       └──► 記憶體位址流 ──► Cache/TLB 前置模擬 ──► 延遲欄位          ║
║                                    │                                 ║
║                                    ▼                                 ║
║                          ┌──────────────────────┐                    ║
║                          │  .bstf  trace 檔      │  ← 唯一契約       ║
║                          │  (扁平、唯讀、共用)   │                    ║
║                          └──────────────────────┘                    ║
╚══════════════════════════════════════════════════════════════════════╝
                                     │
╔══════════════════════════════════════════════════════════════════════╗
║  編譯層 (Model Compilation)               —— 每次改模型跑一次        ║
║  ──────────────────────────────────────────────────────────────────  ║
║   model.v ──► Yosys ──► .json netlist ──► bstm-cc ──┬──► model_bs.c  ║
║      │        (word-level cells)                     ├──► model.metal║
║      │                                               └──► model.cu   ║
║      └──────► Verilator ──► 單 instance 黃金參考模型                  ║
╚══════════════════════════════════════════════════════════════════════╝
                                     │
╔══════════════════════════════════════════════════════════════════════╗
║  執行層 (Runtime)                          —— 每次 DSE sweep 跑      ║
║  ──────────────────────────────────────────────────────────────────  ║
║   config matrix ──► instance 打包 ──► worker 執行緒池                ║
║                          │                                           ║
║                    ┌─────┴─────┬─────────┬─────────┐                 ║
║                    ▼           ▼         ▼         ▼                 ║
║                 core 0      core 1    core 2   ... core 15           ║
║                 64 lanes    64 lanes  64 lanes                       ║
║                    │           │         │         │                 ║
║                    └─────┬─────┴─────────┴─────────┘                 ║
║                          ▼                                           ║
║                 per-instance counters ──► .parquet / .csv            ║
╚══════════════════════════════════════════════════════════════════════╝
```

## 4.2 單一 worker 內部的資料流

這是整個系統最核心的迴圈，值得單獨畫出來：

```
                    ┌────────────────────────────────────────┐
                    │      .bstf trace（unified memory）       │
                    │   唯讀、所有 instance 共用、cache 友善   │
                    └────────────┬───────────────────────────┘
                                 │  scalar gather（每 8~16 cycle 一次）
                                 ▼
        ╔════════════════════════════════════════════════════╗
        ║   Refill 區（scalar 程式碼，非 bit-sliced）          ║
        ║   ─────────────────────────────────────────────    ║
        ║   for lane in 0..63:                               ║
        ║       讀 lane 的 trace cursor                       ║
        ║       抓 M 筆 record                                ║
        ║   bit-transpose：64×W → W 個 word                   ║
        ╚════════════════════╤═══════════════════════════════╝
                             ▼
              ┌──────────────────────────────┐
              │   Fetch Buffer（bit-sliced） │  ← scalar/vector 邊界
              │   深度 M，每 lane 一份游標    │
              └──────────────┬───────────────┘
                             ▼
        ╔════════════════════════════════════════════════════╗
        ║   Timing Model（bit-sliced，純 bitwise，無分支）    ║
        ║   ─────────────────────────────────────────────    ║
        ║   Fetch → Decode → Rename → Issue → Exec → Commit  ║
        ║          每個 target cycle 呼叫一次 eval_cycle()    ║
        ║          每個 word 同時代表 64 個 instance          ║
        ╚════════════════════╤═══════════════════════════════╝
                             ▼
              ┌──────────────────────────────┐
              │  Counters（bit-sliced 累加）  │
              │  cycle / retire / stall 分類  │
              └──────────────┬───────────────┘
                             ▼  結束時 transpose 回 scalar
                    ┌────────────────────┐
                    │  64 組獨立統計結果  │
                    └────────────────────┘
```

**關鍵設計：Fetch Buffer 就是 scalar 與 vector 的邊界。**

這解決了 bit-slicing 最根本的困難：64 個 lane 會在 trace 上走到不同位置（A 卡住了、B 沒有），而 bit-sliced 程式碼做不了 per-lane 的 gather。解法是給每個 instance 一個 fetch buffer —— buffer 本身是 bit-sliced state，模型正常讀它；buffer 快空的時候跳出 vector 區，用 scalar 程式碼做 64 次 gather 加一次 bit transpose 填滿它。

這個東西**本來就是真 CPU 有的**（fetch queue），模型裡本來就該有。它同時當作架構元件和實作邊界，是這個設計最漂亮的地方。

## 4.3 各層的更新頻率

理解各層的更新頻率，對開發節奏很重要：

| 層 | 觸發條件 | 耗時量級 | 產物 |
|---|---|---|---|
| Spike trace 產生 | 換 workload / 換 SimPoint | 數十分鐘 ~ 數小時 | `.bstf`（GB 級） |
| Cache 前置模擬 | 換 cache 配置 | 數分鐘 | `.bstf` 的延遲欄位 |
| Yosys + bstm-cc | 改模型 Verilog | 數秒 ~ 數分鐘 | `model_bs.c` |
| C 編譯 | 同上 | 數秒 ~ 數分鐘 | `.so` / 執行檔 |
| DSE sweep | 每次實驗 | 分鐘 ~ 小時 | 統計結果 |

注意 trace 產生是**最貴但最少變動**的一層。一份 trace 可以服務成千上萬次 DSE sweep。這個不對稱性是整個架構划算的根本原因。

---

# 5. 模型層：A-Port 抽象與建模規範

## 5.1 為什麼要 A-Port

Pellauer 等人的 A-Port Networks（FPGA 2008 / TRETS 2009）是專為「把 timing model 跑在平行 host 上」設計的抽象。它的自我描述是：*a simulation scheme designed to expose the fine-grained parallelism inherent in performance models*。

規則只有三條：

1. timing model = 一堆 module，用 **port** 連起來，每個 port 有**固定 latency 與 buffer 深度**
2. 每個 module 每個 target cycle **恰好 fire 一次**：讀 port → 算 → 寫 port
3. module 之間**沒有任何其他通訊管道**

帶出來的性質正是 BSTM 需要的：

- **沒有 event queue** —— time-stepped 而非 event-driven。GPU 與 SIMD 最大的死穴就是非同步時間推進，A-Port 直接繞過它。
- **靜態排程** —— 每個 cycle 跑哪些 module、什麼順序，編譯期就決定。
- **通訊圖固定** —— 可以預先做 layout 與 liveness 分析。
- **形式上可證明 cycle-exact** —— LI-BDN（Vijayaraghavan & Arvind）是同一件事的形式化版本，FireSim 的 Golden Gate 建於其上。

換句話說：**一個 A-Port timing model 就是一張 synchronous dataflow graph**，而這正是 bit-slicing 與 SIMD 最會吃的形狀。

## 5.2 在 Verilog 裡怎麼寫 A-Port

不需要新語言。用一個簡單的 convention 即可：

```verilog
// A-Port 的標準介面：valid / ready / payload，固定一拍 latency
module aport #(parameter W = 8, parameter DEPTH = 2) (
    input  wire         clk, rst,
    input  wire         snd_valid,  output wire snd_ready,
    input  wire [W-1:0] snd_data,
    output wire         rcv_valid,  input  wire rcv_ready,
    output wire [W-1:0] rcv_data
);
    // 固定深度的 skid buffer，不可繞過
endmodule
```

**建模規約（lint 規則可自動檢查）**：

- 所有跨 module 的訊號必須走 `aport`，禁止直接 wire 連接
- 禁止組合迴路（Yosys `check -assert` 會抓）
- 禁止跨 module 的組合路徑（確保每個 module 可獨立排程）
- 單一 clock domain，同步 reset

## 5.3 建模規範：五條硬規則

這五條不是建議，是**違反就會讓整個方案失效**的硬規則。應該寫成 lint 檢查，CI 強制。

### 規則 1：欄位盡量窄，禁止寬算術

這條有實測支撐。同一個 830-cell 設計，把 counter 從 4-bit 改成 16-bit：

| | Verilator | BSTM |
|---|---|---|
| 4-bit | 5.99 M/s | 3.24 M/s |
| 16-bit | 5.91 M/s（**幾乎不變**） | 1.07 M/s（**掉 3 倍**） |

原因是 cell 展開的成本正比於位元寬度：

| cell | Verilator | bit-sliced | 稅 |
|---|---|---|---|
| `$and`/`$or`/`$not`（1 bit） | 1 op | 1 op | **0** |
| `$mux`（1 bit） | 1 op | 3 ops | 3× |
| `$eq`（W bit） | 1 op | ~2W ops | 2W |
| `$add`/`$sub`（W bit） | 1 op | ~5W ops（ripple carry） | **5W** |

**具體要求**：
- queue pointer、tag、counter 一律 ≤ 8 bit
- 絕對不在模型裡出現 32/64-bit 算術
- 地址、PC、資料值**完全不進模型** —— 需要的話由 trace 提供已算好的結果

如果你發現模型需要寬算術，那代表有東西該搬到 ISS 側或前置層去算。這條規則同時是效能規則和架構規則。

### 規則 2：最大尺寸模型 + runtime mask

bit-slicing 要求 64 個 lane 走**完全相同的程式碼路徑**。所以：

- **可以 bit-slice**：只改參數值的配置（latency、threshold、predictor 表格內容、enable bit、**在最大值以內的尺寸**）
- **不能 bit-slice**：改變 netlist 結構的配置（issue width、執行單元數量、bypass 拓樸）

解法是把尺寸做成 runtime 可控：

```verilog
module rob #(parameter MAX_ENTRIES = 256) (
    input wire [7:0] cfg_entries,   // ← 每個 lane 可以不同
    ...
);
    // 永遠建 MAX_ENTRIES 個 entry
    // 用 (idx < cfg_entries) 當 mask 關掉多餘的
```

代價是永遠付最大尺寸的模擬成本。但因為成本是線性的而非指數的，而你換到的是「一個 batch 掃 64 種尺寸」，這個交換是划算的。

**這條規則必須在寫第一行模型程式碼之前決定，事後補會非常痛。**

### 規則 3：純同步，無 latch、無多 clock domain

Yosys 的 `$sdffe` 映射乾淨，latch 與非同步邏輯會讓 backend 複雜度爆炸。單一 clock domain 也讓「一個 target cycle = 一次 `eval_cycle()`」這個模型成立。

### 規則 4：`case` 一律寫 `default`，避免 `$pmux` 陷阱

Yosys 會把 `case` 轉成 `$pmux`（one-hot 選擇）。如果 select 不是真的 one-hot（條件重疊或缺 `default`），bit-sliced 展開的行為會跟 RTL 模擬不一致 —— 而且是靜默地不一致。

建議在 Yosys 流程加 `pmuxtree` 轉成二元 mux 樹，或在 lint 階段強制檢查所有 `case` 完整。

### 規則 5：所有統計 counter 必須在模型內、bit-sliced

不能用 `$display` —— bit-sliced 版本沒有它。所有想觀測的東西（retire 數、各類 stall cycle、queue 佔用率）都要做成模型裡的 counter，執行期再 transpose 回 scalar。

這條看似麻煩，但有個好處：**counter 是模型的一部分，Verilator 參考模型也會算同一組，對拍時自動一起驗證**。

## 5.4 模型的建議結構

```
top
 ├── frontend
 │    ├── fetch          （讀 fetch buffer，處理 wrong-path 切換）
 │    ├── bpu            （predictor 狀態機 + 表格）
 │    └── decode_queue
 ├── rename
 │    ├── rat            （最大尺寸 + mask）
 │    └── freelist
 ├── backend
 │    ├── issue_queue    （wakeup/select，這裡最深、最貴）
 │    ├── exec_units     （只有延遲倒數，沒有任何運算）
 │    └── rob
 ├── lsu
 │    ├── ldq / stq      （age-ordered disambiguation）
 │    └── mem_latency    （讀 trace 已解析的延遲欄位）
 └── counters            （全域統計）
```

注意 `exec_units` **只有延遲倒數，沒有任何運算** —— 這正是「主要核心計算交給 ISS」的具體落地。ALU 在 BSTM 裡就是一個 4-bit 倒數計數器。

---

# 6. 編譯鏈：Verilog → Yosys → bit-sliced backend

## 6.1 完整流程

```
  model.v  (+ 建模規範 lint)
     │
     ├─────────────────────────────────────────────────┐
     │                                                 │
     ▼                                                 ▼
  ┌──────────────────────────┐              ┌────────────────────┐
  │ Yosys                    │              │ Verilator          │
  │  read_verilog            │              │  --cc --build      │
  │  hierarchy -check -top   │              │                    │
  │  proc                    │              │  單 instance       │
  │  opt_expr; opt_clean     │              │  word-level        │
  │  pmuxtree                │              │  可 $display       │
  │  flatten                 │              │  可 dump 波形      │
  │  opt -fast               │              └─────────┬──────────┘
  │  write_json              │                        │
  └──────────┬───────────────┘                        │
             │ model.json                             │ 黃金參考
             ▼                                        │
  ┌──────────────────────────┐                        │
  │ bstm-cc（本專案）         │                        │
  │  1. 解析 JSON             │                        │
  │  2. 建 net 相依圖         │                        │
  │  3. 拓樸排序              │                        │
  │  4. liveness + slot 配置  │                        │
  │  5. cell → bitwise 展開   │                        │
  │  6. 產生目標程式碼        │                        │
  └──────────┬───────────────┘                        │
             │                                        │
     ┌───────┼────────┬──────────────┐                │
     ▼       ▼        ▼              ▼                │
  model_bs.c  .metal  .cu      (未來 backend)          │
     │                                                │
     └────────────────► 逐 cycle bit-exact 對拍 ◄──────┘
```

## 6.2 Yosys 到底輸出什麼

這是常見的疑問，值得具體說明。Yosys `write_json` 輸出的是**一張 word-level cell 網表**，不是程式碼、不是 gate-level，是一份保留了運算子語意的結構化資料。

以一個 53 行的 timing model 片段（一級 pipeline stage + valid/stall 握手 + 延遲倒數 + credit counter）為例，跑完 `proc / opt / flatten` 得到 **25 個 cell**：

```
$sdffe  4   ← 暫存器（帶 enable + 同步 reset）
$mux    4       $and  4      $reduce_bool 5
$sub    2       $eq   2      $add 1
$not    1       $pmux 1      $logic_not 1
```

注意它**沒有**把 `credit - 1` 拆成一堆 AND/INVERT —— 那仍然是一個 4-bit 的 `$sub`。這就是「停在 word level，不做 techmap/abc」的意思。

JSON 裡的表示：net 是整數 ID，cell 是 `{type, parameters, connections}`：

```json
"$sub$stage.v:43$11": {
  "type": "$sub",
  "parameters": { "A_WIDTH": 4, "B_WIDTH": 1, "Y_WIDTH": 4 },
  "connections": { "A": [49,50,51,52], "B": ["1"], "Y": [65,66,67,68] }
},
"$procmux$24": {
  "type": "$mux",
  "parameters": { "WIDTH": 4 },
  "connections": { "A": [61,62,63,64], "B": [65,66,67,68],
                   "S": [34], "Y": [44,45,46,47] }
}
```

這個格式好用的原因：
1. net 是整數，可以直接當陣列索引
2. 每個 cell 是一個「幾行程式碼就能展開」的算子
3. 已經 flatten 過，沒有階層
4. 連線是 bit list（LSB first），bit-slice 展開是機械式的

## 6.3 bstm-cc 的核心：cell dispatch table

backend 的本體就是一張表：

| cell | 每個 bit 展開成 |
|---|---|
| `$and` | `Y[i] = A[i] & B[i];` |
| `$or` | `Y[i] = A[i] \| B[i];` |
| `$xor` | `Y[i] = A[i] ^ B[i];` |
| `$not` | `Y[i] = ~A[i];` |
| `$mux` | `Y[i] = (A[i] & ~S) \| (B[i] & S);` |
| `$pmux` | `Y[i] = (B[0*W+i] & S[0]) \| (B[1*W+i] & S[1]) \| ...;` |
| `$eq` | `Y = ~((A0^B0) \| (A1^B1) \| ...);` |
| `$reduce_bool`（`!=0`） | `Y = A0 \| A1 \| A2 \| ...;` |
| `$logic_not`（`==0`） | `Y = ~(A0 \| A1 \| ...);` |
| `$add` / `$sub` | ripple carry：`Y=a^b^c; c=(a&b)\|(c&(a^b));` |
| `$sdffe` | `s->q = (D & en) \| (p->q & ~en);` |
| `$memrd` / `$memwr` | 見 6.5 |

**沒有 if，沒有分支** —— 全部是 predicated 的 bitwise 運算。

產生出來的 C 實際長這樣：

```c
typedef uint64_t vec_t;                /* 64 lanes = 64 個 instance */

n35 = ~(n49 | n50 | n51 | n52);        /* cnt == 0        */
n33 =   n49 | n50 | n51 | n52;         /* cnt != 0        */
n16 = ~n32;                            /* in_ready = ~v   */
{   /* credit - 1 : ripple carry, 4 bit */
    vec_t c = VONES;
    { vec_t a=n49, b=~VONES; n65 = a^b^c; c = (a&b)|(c&(a^b)); }
    { vec_t a=n50, b= VONES; n66 = a^b^c; c = (a&b)|(c&(a^b)); }
    ...
}
n34 = n32 & n33;                       /* busy      */
n17 = n36 & n37;                       /* can_issue */
n44 = (n61 & ~n34) | (n65 & n34);      /* mux       */
/* FF 更新 */
s->q49 = (n44 & (n48 & ~n3)) | (p->q49 & ~(n48 & ~n3) & ~n3);
```

**每一行同時算 64 個 instance。**

原型驗證：一支 **167 行的 Python** 涵蓋 `$and/$or/$xor/$not/$mux/$pmux/$eq/$reduce_bool/$logic_not/$add/$sub/$sdffe` 加上拓樸排序，產出的 C 編得過、跑得動、效能符合預期。這是整條路徑風險最低的一環。

## 6.4 必須補的工程項目

原型能跑，但要上線還缺以下幾項。這些是 bstm-cc 的實際工作量所在：

### (a) Liveness 分析與 slot 重用【高優先】

原型把**每個 net 都宣告成一個區域變數**。對 830 cell 的設計沒問題，但對一個兩萬 cell 的真實模型，這會產生兩萬多個 `vec_t` 區域變數 —— 編譯器會大量 spill，效能崩潰，編譯時間也會爆炸。

正確做法是對 net 圖做 liveness 分析，把生命期不重疊的 net 配到同一個 slot：

```
  net 生命期分析 ──► 干涉圖 ──► 圖著色 ──► slot 陣列
  
  預期效果：兩萬個 net → 數百個 slot
           工作集從 ~10 MB 降到 ~數百 KB，塞進 L1/L2
```

這是 bstm-cc 最重要的一個優化，直接決定方案在真實規模下能不能成立。

### (b) 常數傳播與 strength reduction

原型產生的程式碼裡有大量 `(VZERO & ~S) | (B & S)` 這種可以直接化簡成 `B & S` 的式子。雖然 gcc `-O3` 會處理掉一部分，但在 backend 做可以大幅減少產生的程式碼量與編譯時間。

### (c) 多 backend 抽象

把 word 型別與基本操作抽象成一層，讓同一份 IR 可以出多個目標：

| 目標 | word 型別 | lane 數 |
|---|---|---|
| 純 C（除錯用） | `uint64_t` | 64 |
| ARM NEON | `uint32x4_t` | 128 |
| x86 AVX-512 | `__m512i` | 512 |
| Metal | `uint4` | 128 |
| CUDA | `uint4` | 128 |

### (d) 加法器的進位鏈優化

ripple carry 的序列相依會殺掉 ILP。對 8-bit 以上的欄位，應該產生 carry-lookahead 或 carry-select 形式，用面積換延遲。不過依照規則 1，模型裡不該有寬欄位，所以這項優先度低。

### (e) `x` 語意

bit-sliced 是純 2-state。Verilator 參考模型要用 `--x-assign 0 --x-initial 0` 才能對得起來。這必須寫進對拍腳本，否則會浪費大量時間追查假的不一致。

## 6.5 記憶體結構的處理（關鍵風險項）

這是編譯鏈最大的風險。timing model 裡有大量「小型多埠結構」：

- ROB（多讀多寫）
- issue queue 的 wakeup CAM（全對全比對）
- RAT（每個 rename port 一個讀埠）
- branch predictor 表格

Yosys 的 `memory_libmap` 會嘗試把這些映射成 RAM，但**多埠非同步讀取的結構會退化成 FF + mux tree**，造成閘數與邏輯深度爆炸。GEM 論文明確報告了這個 failure mode：五個 benchmark 裡只有 NVDLA 的 RAM 全部原生映射成功，其他四個都踩到。

**緩解策略（按優先順序）**：

1. **改寫成同步讀取。** 大多數 timing model 的結構可以接受一拍讀取延遲 —— 反正真硬體也是。這是最乾淨的解法。
2. **wakeup CAM 改成 bit-matrix。** 與其做 N×M 的 tag 比對，不如維護一個 dependency bit matrix，每個 bit 是 `uint64_t` 的一個 lane。這在 bit-sliced 形式下反而非常便宜 —— 一個 64-entry 的 wakeup 就是幾個 word 的 AND/OR。**這是 bit-slicing 意外的優勢，值得專門設計。**
3. **表格用 `$memrd` 同步讀，由 backend 展成 per-lane gather。** 放在 refill 區（scalar）處理，跟 fetch buffer 同一個機制。
4. **小表格直接展成暫存器陣列。** 對 ≤ 16 entry 的結構這樣最快。

**Phase 0 必須先驗證這一項**：把你打算用的 ROB/IQ/RAT 寫法丟進 Yosys `memory_libmap`，看映射結果。如果大量出現 `$__RAMGEM_ASYNC_` 或 `using FF mapping for memory`，就要在建模階段改寫，而不是等到編譯鏈做完才發現。

---

# 7. ISS 整合：Spike 前置管線與 trace 契約

## 7.1 架構取捨：為什麼退回 functional-first

原始的 `nds_spiny_sim` 是 timing-directed：pipeline 在尾端呼叫 Spike 決定 branch 與後續指令流。這個設計精度最好，但**跟高吞吐是不相容的**：

- 每個 target cycle 要做一次 host 函式呼叫 → 無法 bit-slice（64 個 lane 各自要呼叫）
- Spike 的狀態是 per-instance 的 → 64 份 Spike 實例，記憶體與快取壓力爆炸
- 無法向量化、無法移植到 GPU

**BSTM 刻意退回 functional-first，並用「預先展開 wrong-path」補回損失的能力。**

```
   timing-directed（原設計）            functional-first（BSTM）
   ─────────────────────────           ─────────────────────────
   ┌─────────┐   每 cycle              ┌─────────┐  離線一次
   │ pipeline│ ◄──────────► ┌───────┐  │ Spike   │ ────────►  .bstf
   │  model  │   函式呼叫    │ Spike │  └─────────┘   (含 wrong-path)
   └─────────┘              └───────┘                     │
                                                          ▼
   ✓ 精確的 wrong-path                            ┌─────────────┐
   ✓ 支援自我修改程式碼                            │ pipeline    │
   ✗ 無法 bit-slice                               │ model × 64  │
   ✗ 無法向量化                                    └─────────────┘
   ✗ 每 lane 一份 Spike                            ✓ 完全可 bit-slice
                                                   ✓ trace 唯讀共用
                                                   ✗ wrong-path 需預先展開
                                                   ✗ 無法處理自我修改程式碼
```

## 7.2 `.bstf` 檔案格式（Bit-Sliced Timing Format）

trace 裡放的**不是指令，是「timing model 需要知道的事」**。這個區分很重要：模型不需要知道 `add x1, x2, x3` 這件事，它只需要知道「這是一個 ALU 類、吃兩個來源暫存器、寫一個目的暫存器、延遲 1 拍的動作」。

### 檔頭

```c
struct bstf_header {
    char     magic[8];        // "BSTF\0\0\0\1"
    uint32_t version;
    uint32_t record_bytes;    // 16
    uint64_t n_records;       // correct-path 記錄數
    uint64_t n_shadow;        // wrong-path 記錄數
    uint64_t shadow_offset;   // shadow 區起始
    uint32_t simpoint_id;
    uint32_t simpoint_weight; // SimPoint 權重（定點數）
    uint64_t icount_start;    // 在原始執行中的位置
    char     workload[64];
    char     cache_cfg[64];   // 前置模擬用的 cache 配置指紋
};
```

### 記錄格式（16 bytes）

```
 bit  位置   欄位              說明
 ───────────────────────────────────────────────────────────────
  0    [3:0]  uop_class        ALU / MUL / DIV / FP / LD / ST / BR / CSR / FENCE
  4    [4:0]  src1             架構暫存器編號（31 = 無）
  9    [4:0]  src2
 14    [4:0]  dst
 19    [0:0]  src1_valid
 20    [0:0]  src2_valid
 21    [0:0]  dst_valid
 22    [3:0]  exec_latency     執行單元佔用拍數（由 uop_class 決定，預先算好）
 26    [0:0]  is_branch
 27    [0:0]  br_taken         實際方向（ground truth）
 28    [0:1]  br_type          cond / jal / jalr / ret
 30    [7:0]  mem_latency      ★ 已解析的記憶體延遲（見 7.4）
 38    [0:0]  mem_is_store
 39    [2:0]  mem_size
 42    [15:0] bpu_index        predictor 索引用的 PC hash（預先算好）
 58    [15:0] br_target_hash   BTB 比對用的目標 hash
 74    [23:0] shadow_index     wrong-path shadow 的起始位置（0 = 無）
 98    [5:0]  shadow_len       shadow 長度
104    [7:0]  flags            serialize / fence / trap / simpoint_end
112    [15:0] reserved
 ───────────────────────────────────────────────────────────────
                                                    合計 128 bit = 16 bytes
```

**設計原則**：所有能預先算的東西都預先算。PC 不進 trace（改成 hash），地址不進 trace（改成延遲），opcode 不進 trace（改成 class）。模型永遠不做算術，只做比較與計數。

## 7.3 wrong-path 的預先展開

這是 functional-first 最困難的部分，也是本專案已知的最大技術風險之一（詳見第 11 章）。

**機制**：

```
   correct path:  ... I(n-1)  I(n)=branch  I(n+1)  I(n+2) ...
                                    │
                                    │ shadow_index
                                    ▼
   shadow 區:                 S0 S1 S2 ... S(K-1)
                              （沿著「與實際方向相反」那條路徑展開 K 條）
```

模型的行為：
1. 預測器給出預測方向
2. 若預測 == `br_taken`（正確），繼續讀 correct path
3. 若預測 != `br_taken`（錯誤），fetch 單元切換到 `shadow_index` 開始讀 shadow
4. 走 `shadow_len` 條（或直到 branch 解析）之後 flush，回到 correct path

**K 的選擇**：K 應該 ≥ 你的 misprediction penalty window，也就是「從 fetch 到 branch 解析」期間能塞進 pipeline 的最大指令數。對一個 10 級深、4-wide 的設計，K ≈ 40 是合理的。K 太小會低估 wrong-path 的資源佔用；K 太大則 trace 膨脹。

**trace 膨脹估算**：假設 20% 的指令是 branch，K = 40，則 shadow 區大小 ≈ correct path × 0.2 × 40 = **8 倍**。這是可觀的成本，但用 128 GB 記憶體可以吸收（見 7.5）。

**降低膨脹的手段**：
- 只對「預測可能出錯」的 branch 展開 —— 先用一個簡單的 predictor 跑一遍 profile，對總是預測正確的 branch 不展開（可省 80% 以上）
- shadow 之間共用尾段（很多錯誤路徑會很快匯流）

## 7.4 記憶體階層的前置解析

**這是 BSTM 最大的簡化，也是最大的準確度妥協。**

做法：在離線階段用獨立的 trace-driven cache simulator 跑一遍位址流，把每個 load/store 的 hit/miss 與延遲寫進 trace 的 `mem_latency` 欄位。timing model 完全不模擬 cache，只讀那個數字。

```
  Spike ──► 位址流 ──► Cache Sim ──► 每筆 access 的延遲
                          │
                     （可用 inclusion property
                       一次算多組 cache 配置）
```

**收益**：砍掉整個 tag array、replacement state、MSHR —— 也就是模型裡唯一不規則、state 重、完全不適合 bit-slice 的部分。

**代價（必須誠實記錄）**：
- 失去 **MSHR 競爭**建模
- 失去 **bank conflict**
- 失去 **memory-level parallelism（MLP）** —— 而現代 OOO core 的效能主要由 MLP 決定
- 失去 pipeline 回饋到記憶體系統的效應（prefetcher 被 access stream 訓練，而 access stream 取決於時序）
- cache 配置與 pipeline 配置不能在同一個 batch 裡同時掃（要掃就在前處理做 cross product）

**這是本方案最嚴重的準確度洞。** 第 11 章有緩解計劃。簡短版本：Phase 3 要把一個**簡化的 MLP 模型**（MSHR 計數 + 未完成 miss 佇列）放回 timing model，這部分欄位窄、可以 bit-slice，能補回大部分損失。

## 7.5 記憶體容量規劃（128 GB 的用處）

這是 Mac M5 Max 128 GB 在這個工作負載上最重要的規格 —— 比核心數重要。

```
  一個 SimPoint interval：        10 ~ 100 M 指令
  每筆記錄：                      16 bytes
  ────────────────────────────────────────────
  correct path / interval：       0.16 ~ 1.6 GB
  wrong-path shadow（優化後 ~2×）：0.3  ~ 3.2 GB
  ────────────────────────────────────────────
  單一 interval 合計：             0.5  ~ 4.8 GB
  30 個 interval：                 15   ~ 144 GB
```

以 50 M 指令 / interval、30 個 interval 計算，約 **48 GB** —— 128 GB 綽綽有餘，而且 **CPU 與 GPU 看到的是同一塊**（unified memory）。一張 24 GB 的 discrete GPU 做不到這件事。

**所有 instance 共用同一份 trace（唯讀）** —— 不同配置只是消耗速率不同、走的 wrong-path 不同。所以 trace 是 read-mostly、cache 友善的，不會變成頻寬瓶頸。

## 7.6 Spike 側的實作

利用既有的 makatau cosim 經驗，Spike 側的工作相對單純：

1. 在 `commit_log` 的鉤子上加一個 `.bstf` writer
2. 用 `--isa` 與 `--pc` 控制起訖點，配合 SimPoint 選出的區間
3. wrong-path 展開需要一個「從任意 PC 開始執行 K 條、不 commit 狀態」的模式 —— 用 Spike 的 checkpoint / restore 或 fork 實作
4. BBV 產生用現成的 Spike-STF 或自己加一個 basic-block counter

**注意事項**：
- 需要 warmup。SimPoint 區間開始前要先跑一段讓 cache/predictor 暖機，或用 checkpoint 帶入暖機後的狀態
- `jalr` / `ret` 的 wrong-path 無法預先展開（不知道預測目標）—— 見第 11 章

---

# 8. 執行期：instance 佈局、排程與結果收集

## 8.1 instance 的定義與維度

一個 **instance** 是「一組具體配置 × 一個 SimPoint 區間」的組合：

```
   instance = (config_id, simpoint_id)
   
   config_id  ← DSE 掃描的參數組合（ROB 大小、IQ 大小、predictor 參數…）
   simpoint_id ← 代表性區間編號（含權重）
```

總 instance 數 = `n_config × n_simpoint`。例如 100 個配置 × 30 個區間 = **3,000 個 instance**。

## 8.2 lane 的打包策略

哪些 instance 可以放進同一個 64-lane batch？**必須共用同一份編譯出來的模型**。因此：

```
   ┌── 可以同 batch ────────────────────────────────────┐
   │  • 不同 config_id（只要都是參數性差異）             │
   │  • 不同 simpoint_id（trace 不同，但模型相同）       │
   └────────────────────────────────────────────────────┘
   ┌── 不可同 batch ──────────────────────────────────┐
   │  • 結構性差異（issue width、EU 數量、bypass 拓樸） │
   │    → 必須分開編譯，各自一個 batch 集合             │
   └────────────────────────────────────────────────────┘
```

**打包建議**：優先把「同一個 simpoint、不同 config」放同一個 batch。理由是它們共用同一份 trace 記憶體，L2 命中率最高。

```
   batch 0:  simpoint 0  × config 0..63
   batch 1:  simpoint 0  × config 64..99  (+ 28 個空 lane)
   batch 2:  simpoint 1  × config 0..63
   ...
```

空 lane 的浪費可以用「把小的 simpoint 補進來填滿」來降低，但實作複雜度不值得 —— 先接受最多 50% 的 lane 浪費。

## 8.3 執行緒池與工作分配

```
   ┌───────────────────────────────────────────────────────┐
   │  主執行緒                                              │
   │   • 讀 config matrix                                   │
   │   • mmap 所有 .bstf（唯讀、MAP_SHARED）                │
   │   • 建 batch 佇列                                      │
   └──────────────────────┬────────────────────────────────┘
                          │  work-stealing queue
      ┌───────────┬───────┴───────┬───────────┬─────────┐
      ▼           ▼               ▼           ▼         ▼
   worker 0    worker 1        worker 2   ...      worker 15
   ┌────────┐  ┌────────┐      ┌────────┐         ┌────────┐
   │batch A │  │batch B │      │batch C │         │batch P │
   │64 lane │  │64 lane │      │64 lane │         │64 lane │
   │state:  │  │        │      │        │         │        │
   │ ~2 MB  │  │        │      │        │         │        │
   └────────┘  └────────┘      └────────┘         └────────┘
      │           │               │                   │
      └───────────┴───────┬───────┴───────────────────┘
                          ▼
                 結果聚合（per-instance counters）
```

**設計要點**：
- 每個 worker 的 state 必須塞進該核的 L2。這是 liveness 優化（6.4a）的動機所在。
- 使用 work-stealing 而非靜態分配：不同 config 的執行時間差異可能到 2 倍（stall 多的跑比較久？不會 —— oblivious full-cycle 的每 cycle 成本固定，但 instance 的**總 cycle 數**不同，因為 IPC 不同）。
- M5 Max 有 18 核，建議用 16 個 worker，留 2 核給 I/O 與作業系統。

## 8.4 執行的主迴圈

```c
void run_batch(batch_t *b) {
    state_t cur = {0}, nxt = {0};
    init_config(&cur, b->configs);          /* 每 lane 不同的參數 */
    
    uint64_t target_cycle = 0;
    while (!all_lanes_done(&cur)) {
        /* ── scalar 區：補 fetch buffer ── */
        if (any_lane_needs_refill(&cur)) {
            refill_fetch_buffers(b, &cur);  /* gather + bit transpose */
        }
        
        /* ── vector 區：純 bitwise，無分支 ── */
        eval_cycle(&nxt, &cur);
        
        swap(&cur, &nxt);
        target_cycle++;
        
        /* ── 終止檢查（每 1024 cycle 做一次，攤銷成本）── */
        if ((target_cycle & 1023) == 0)
            update_done_mask(&cur);
    }
    
    extract_counters(b, &cur);              /* transpose 回 scalar */
}
```

**注意 `all_lanes_done`**：不同 lane 會在不同 target cycle 完成（IPC 不同）。已完成的 lane 要用 mask 凍結（`cfg_enable` 設 0），避免污染統計。整個 batch 要跑到**最慢的那個 lane**結束為止。

這代表 batch 內的 IPC 差異越大，浪費越多。**打包時可以考慮把預期 IPC 相近的 config 放在一起**，但這需要先有一輪粗略結果 —— 適合當第二輪優化。

## 8.5 Refill 的實作細節

這是 scalar/vector 邊界，也是效能關鍵之一。

```c
void refill_fetch_buffers(batch_t *b, state_t *s) {
    /* 1. 從 bit-sliced 取出每個 lane 的 cursor（transpose out） */
    uint32_t cursor[LANES];
    unpack_lanes(s->fetch_cursor, cursor, CURSOR_BITS);
    
    /* 2. scalar gather：每個 lane 抓 M 筆記錄 */
    bstf_rec_t tmp[LANES][REFILL_M];
    for (int l = 0; l < LANES; l++) {
        const bstf_rec_t *src = b->trace[l];        /* 該 lane 的 trace 基底 */
        for (int m = 0; m < REFILL_M; m++)
            tmp[l][m] = src[cursor[l] + m];         /* 可能落在 shadow 區 */
    }
    
    /* 3. bit transpose：LANES × BITS → BITS 個 word */
    for (int m = 0; m < REFILL_M; m++)
        transpose64(tmp, m, &s->fetch_buf[m]);
    
    /* 4. 更新 cursor（回到 bit-sliced 形式） */
    pack_lanes(cursor, s->fetch_cursor, CURSOR_BITS);
}
```

**成本攤銷**：`REFILL_M` 決定攤銷倍率。若 REFILL_M = 16 且模型每 cycle 最多消耗 4 筆，則每 4 個 cycle 才 refill 一次。實務上建議 REFILL_M = 32。

**bit transpose 的實作**：用 Hacker's Delight 的 64×64 遞迴交換法，或 ARM NEON 的 `vtrn`/`vzip` 系列。這段值得手寫優化，因為它是唯一無法避免的 scalar 成本。

## 8.6 統計的收集

所有 counter 在模型內部是 bit-sliced 的整數。例如一個 48-bit 的 cycle counter 就是 48 個 `vec_t`。

```
   模型內：  cycle_cnt[0..47]   每個是一個 64-bit word
                  ↓  結束時 transpose
   scalar：  uint64_t cycle[64]  每個 lane 一個值
```

**要收集的最小集合**：

| counter | 用途 |
|---|---|
| `total_cycles` | IPC 分母 |
| `retired_uops` | IPC 分子 |
| `fetch_stall_*` | 前端瓶頸分類（icache / bpu / buffer full） |
| `issue_stall_*` | 後端瓶頸分類（IQ full / ROB full / operand not ready） |
| `mem_stall_*` | 記憶體瓶頸 |
| `branch_mispred` | 預測器評估 |
| `rob_occupancy_sum` | 除以 cycle 得平均佔用率 |
| `wrongpath_uops` | 推測執行的浪費量 |

48-bit counter 對 10¹¹ 量級的 cycle 數足夠。注意每個 counter 的 bit-sliced 加法是 ripple carry，成本 = 位元數 × 5 ops，所以 **counter 數量要克制** —— 20 個 48-bit counter 就是 4,800 ops/cycle，可能比模型本體還貴。

**優化**：大部分 counter 是「條件加 1」，可以用「每 64 cycle 做一次 popcount 式的批次累加」來攤銷。或者用較窄的 counter（例如 16-bit）定期 flush 到 scalar 的寬 counter。

---

# 9. 驗證策略

## 9.1 三層驗證

```
   第一層：bit-exact 對拍         ← 每次 commit，CI 強制
   ────────────────────────────────────────────────────
     同一份 model.v
        ├─► Verilator（1 instance，word-level）
        └─► bstm-cc（lane 0，bit-sliced）
     
     逐 cycle 比對所有 output port 與所有 counter
     必須 100% 相同，任何差異即為 bug
   
   第二層：ISS 一致性檢查          ← 每次 trace 重新產生
   ────────────────────────────────────────────────────
     模型 retire 的 uop 序列  vs  Spike 的 commit 序列
     必須完全一致（順序、數量、暫存器編號）
     確保 trace 消費邏輯正確
   
   第三層：相對準確度校準          ← 每個 milestone
   ────────────────────────────────────────────────────
     BSTM  vs  gem5 / RTL（若有）
     同一組 SimPoint、同一個 workload
     看 IPC 絕對值與趨勢方向
```

## 9.2 第一層：bit-exact 對拍（最重要）

這是整個專案的安全網，而且**幾乎是免費的** —— 因為 Verilator 參考模型本來就要有。

```
  ┌─────────────────────────────────────────────────────────┐
  │  測試向量產生                                            │
  │   • LFSR 隨機輸入（快速煙霧測試）                        │
  │   • 真實 .bstf 前 N 筆（真實場景）                       │
  │   • 定向邊界測試（queue 滿/空、同時 flush+fill）         │
  └────────────────────┬────────────────────────────────────┘
                       │
        ┌──────────────┴──────────────┐
        ▼                             ▼
  ┌───────────┐                ┌──────────────┐
  │ Verilator │                │ bstm-cc      │
  │ eval()    │                │ eval_cycle() │
  └─────┬─────┘                └──────┬───────┘
        │  每 cycle 的 output + counter │
        └──────────────┬───────────────┘
                       ▼
                 逐 bit 比對
                 第一個不同 → 報告 cycle 號 + 訊號名
```

**關鍵設定**：Verilator 必須用 `--x-assign 0 --x-initial 0`，否則 X 語意差異會製造大量假警報。

**覆蓋率要求**：對拍要跑到每個 module 的每個狀態都被走過。建議用 Verilator 的 `--coverage` 蒐集 toggle coverage 當作代理指標。

## 9.3 第二層：ISS 一致性

確保 trace 消費邏輯沒有 off-by-one、沒有漏讀、wrong-path 切換正確。

檢查項目：
- retire 總數 == trace record 總數
- 每個 retire 的 `dst` 暫存器編號與 Spike commit log 相同
- wrong-path 的 uop **不可** retire（只能被 flush）
- `shadow_index` 切換後回到 correct path 的位置正確

## 9.4 第三層：相對準確度

這一層不追求絕對準確，追求**趨勢正確**。

```
       IPC
        │        ● BSTM
        │      ●   ○ gem5 / RTL
        │    ●   ○
        │  ●   ○
        │●  ○
        └──────────────────► ROB 大小
        
   可接受：有固定偏移（BSTM 系統性偏高/低）但趨勢單調一致
   不可接受：趨勢方向相反，或交叉
```

理由：DSE 要的是「加 2 way 會不會變好、變多少」，不是絕對 IPC。一個系統性偏 8% 但單調正確的模型，比一個絕對值準但趨勢會翻轉的模型有用得多。

**參考基準**：gem5 O3 對真實 Cortex-A9 的誤差平均 7.4%（範圍 1–18%），對 A15 約 13%；ZSim 對 Westmere 約 9.7%。業界一般把「10% 以內」當作效能模型可用的門檻。BSTM 至少要達到同一個水準，否則沒有存在意義。

## 9.5 效能迴歸

每次 commit 跑一次固定的 benchmark，記錄：
- 每 target-cycle 的耗時
- 產生的程式碼行數與 slot 數
- 編譯時間

任何超過 10% 的退步要有解釋。這條很重要，因為 bit-sliced 的效能對程式碼生成的細節極度敏感，很容易在不知不覺中退化。

---

# 10. 效能模型與量化預估

## 10.1 已完成的實測

原型已經建立並量測。環境：Intel i7-12700，單核，gcc `-O3 -march=native`。

### 測試設計

| 設計 | 描述 | Yosys cell 數 |
|---|---|---|
| `stage` | 一級 pipeline stage + valid/stall 握手 + 4-bit 延遲倒數 + 4-bit credit counter | 25 |
| `chain` | 32 級 `stage` 串接 | 830 |
| `chainw` | 同上，但 counter 改為 16-bit | 831（cell 數相同，位元寬度不同） |

### 結果

| 設計 | Verilator（1 inst） | BSTM（每 target-cycle） | BSTM（64 lane） | 淨勝 |
|---|---|---|---|---|
| `stage`（25 cell） | 27.6 M/s | 164.0 M/s | 10,497 M/s | 380× |
| `chain`（830 cell，4-bit） | **5.99 M/s** | **3.24 M/s** | **208 M/s** | **35×** |
| `chainw`（830 cell，16-bit） | 5.91 M/s | 1.07 M/s | 69 M/s | 12× |

### 三個結論

**(1) 小設計的 benchmark 會得到相反的結論。**

`stage` 只有 25 個 cell，Verilator 的固定 per-`eval()` 開銷（每 cycle 兩次 eval、change detection、symbol table 間接定址）佔主導，看起來 BSTM 每 target-cycle 就快 5.9 倍。放大到 830 cell，開銷攤掉，真實關係才出現：**BSTM 每 target-cycle 比 Verilator 慢 1.8 倍。**

這是一個容易踩的坑，記錄下來避免重蹈。

**(2) 贏的全部來自 lane 倍數。**

```
   淨勝 = lane 數 ÷ 位元序列化稅
   
   4-bit  欄位：64 ÷ 1.8 ≈ 35×
   16-bit 欄位：64 ÷ 5.5 ≈ 12×
```

沒有任何魔法。bit-slicing 是一種**用單 instance 效能換總吞吐**的技術。

**(3) 欄位寬度是可控的主要變數。**

Verilator 從 4-bit 換到 16-bit **完全沒變**（5.99 → 5.91），因為 `cnt - 1` 永遠是一條機器指令。BSTM 掉 **3 倍**。這直接支撐第 5 章的規則 1。

## 10.2 外推到真實規模

真實 OOO timing model 的規模估計：

| 模組 | 估計 cell 數 |
|---|---|
| fetch + fetch buffer | 1,500 |
| branch predictor（TAGE-lite + BTB + RAS） | 4,000 |
| decode + uop queue | 1,500 |
| rename（RAT + freelist） | 3,000 |
| issue queue（wakeup/select，64 entry） | 6,000 |
| ROB（192 entry） | 4,000 |
| LSU（LDQ/STQ + disambiguation） | 4,000 |
| exec units（純延遲倒數） | 800 |
| counters | 1,200 |
| **合計** | **~26,000** |

以 `chain` 的 830 cell / 3.24 M target-cycles/s 線性外推（oblivious full-cycle 的成本正比於 cell 數）：

```
   26,000 / 830 = 31.3 倍
   3.24 M / 31.3 = 0.104 M target-cycles/s
   × 64 lanes    = 6.6 M instance-cycles/s  （單核）
   × 16 cores    = 106 M instance-cycles/s  （M5 Max）
```

**但這個線性外推是樂觀的**，因為沒有考慮工作集超出 L1/L2 的影響。若 liveness 優化（6.4a）做得好，slot 數可以壓到數百個：

```
   500 slots × 64 lanes × 8 bytes = 256 KB   → 塞得進 L2 ✓
   26,000 nets × 64 × 8 = 13.3 MB            → 掉到 L3/記憶體 ✗
```

**這是 liveness 優化為什麼是最高優先項的原因** —— 它決定的不是 20% 的差別，而是兩個數量級。

### 保守與樂觀估計

| 情境 | 單核 | 16 核 |
|---|---|---|
| **悲觀**（liveness 未優化，工作集落在 L3） | 1.5 M | 24 M |
| **中性**（liveness 優化，部分落在 L2） | 6.6 M | 106 M |
| **樂觀**（工作集全在 L1，欄位極窄） | 15 M | 240 M |

單位：instance-cycles/s

## 10.3 換算成 DSE 時間

典型的 sweep：

```
   100 個 config × 30 個 SimPoint × 每個 50 M cycle
   = 1.5 × 10¹¹ instance-cycles
```

| 情境 | 吞吐 | 完成時間 |
|---|---|---|
| 悲觀 | 24 M/s | **1.7 小時** |
| 中性 | 106 M/s | **24 分鐘** |
| 樂觀 | 240 M/s | **10 分鐘** |

**對照組**：

| 方法 | 吞吐 | 同一個 sweep |
|---|---|---|
| Verilator × 16 核 | 96 M/s（實測外推） | 26 分鐘 |
| gem5 O3 × 16 核 | ~5 M/s | **8.3 小時** |
| GEM on A100（單 instance） | 0.05 M/s | 34 天 |

值得注意：**Verilator 跑 16 個獨立 process 的吞吐（96 M/s）跟 BSTM 的中性估計（106 M/s）幾乎一樣。**

這個結果必須誠實面對 —— 它意味著：

> **如果 liveness 優化沒做好，BSTM 相對於「直接開 16 個 Verilator process」的優勢是零。**

BSTM 的優勢只在以下條件同時成立時才顯現：
1. liveness 優化做到位（工作集進 L2）
2. 模型欄位夠窄（稅接近 1.8× 而非 5.5×）
3. 大部分 DSE 維度是參數性的（能塞進同一個 batch）

這三條任何一條不成立，就該直接用 Verilator。**這是本專案最需要早期驗證的事情，也是 Phase 1 決策閘的內容。**

## 10.4 記憶體頻寬檢查

oblivious full-cycle 模擬器每 cycle 要讀過整個狀態。檢查是否會撞到頻寬牆：

```
   中性情境：500 slots × 64 lanes × 8 bytes = 256 KB 工作集
   每 cycle 讀寫約 2 × 256 KB = 512 KB
   × 0.104 M cycles/s = 53 GB/s  （單核）
   × 16 cores         = 853 GB/s
```

M5 Max 的記憶體頻寬是 460–614 GB/s。**853 GB/s 超過了** —— 但因為工作集只有 256 KB，實際上絕大部分命中 L2（M 系列 P-core cluster 的 L2 是 16 MB 共享），不會走到主記憶體。

真正會走主記憶體的是 **trace 讀取**：
```
   每 cycle 每 lane 消耗 ~1.5 筆 record × 16 bytes = 24 bytes
   × 64 lanes × 16 cores × 0.104 M cycles/s = 2.6 GB/s
```
完全不是問題。而且同 batch 的 lane 共用 trace，實際更低。

**結論**：頻寬不是瓶頸，前提是 liveness 優化把工作集壓進 L2。又一次指向同一個關鍵項。

---

# 11. 風險登記冊

這一章刻意寫得悲觀。每一項都是可能讓專案失敗的真實風險，附帶緩解計劃與偵測時機。

## R1｜DSE 維度大多是結構性的【致命 · Phase 0 驗證】

**描述**：bit-slicing 只對「參數性差異」有效。如果你真正想掃的維度大多是結構性的（issue width、執行單元數量、bypass 拓樸、pipeline 級數），每個配置都要獨立編譯，那 64 條 lane 裡放的是 64 份一模一樣的東西 —— 整個方案的價值歸零。

**影響**：專案失去全部意義。

**緩解**：
- Phase 0 先做**維度盤點**（見 12.1），逐項標記參數性 vs 結構性
- 部分結構性維度可以轉成參數性：例如 issue width 建最大值、用 mask 關掉多餘的 port
- 若參數性維度 < 16，建議直接放棄本方案，改用 Verilator 多進程

**偵測時機**：Phase 0，寫任何程式碼之前。

**這是整個計劃的第一個、也是最重要的決策閘。**

---

## R2｜失去 MLP 建模能力【嚴重 · 已知】

**描述**：7.4 把記憶體延遲預先解析進 trace，砍掉了 MSHR 競爭、bank conflict、memory-level parallelism。而現代 OOO core 的效能主要由 MLP 決定 —— 一個 ROB 能不能藏住 miss，取決於它能同時發出幾個未完成的 miss。

**影響**：對 memory-bound workload 的預測會系統性偏樂觀，而且偏差量隨配置變化（ROB 越大偏越多）—— 這會破壞**趨勢正確性**，比絕對誤差嚴重得多。

**緩解**：
- Phase 3 把一個**簡化 MLP 模型**放回 timing model：
  - MSHR 計數器（例如 16 個 entry，4-bit 佔用計數）
  - 未完成 miss 的佇列（每個 entry 一個 8-bit 倒數）
  - miss 時若 MSHR 滿則 stall
- 這部分欄位窄（4–8 bit），完全可以 bit-slice，成本可接受
- trace 側提供 `mem_latency` 仍然有效，只是變成「MSHR 配到之後的延遲」

**偵測時機**：Phase 3 的 gem5 correlation。若 memory-bound benchmark 的誤差 > 20%，這項就是元兇。

---

## R3｜wrong-path 展開不完整【嚴重 · 研究級難題】

**描述**：

1. **間接跳躍與 return 無法預先展開。** `jalr` 的錯誤路徑取決於預測的目標位址，而目標位址取決於 BTB/RAS 狀態，而那取決於配置。你不知道要展開到哪裡去。
2. **wrong-path 的長度取決於配置。** 不同 config 的 misprediction penalty 不同，K 要取最大值。
3. **wrong-path 上的記憶體存取會污染 cache** —— 而 cache 是前置模擬的，看不到 wrong-path。

這不是實作難題，是**開放研究問題**。FAST 論文（2007）之後二十年，2023 年還有人在發 "Simulating Wrong-Path Instructions in Decoupled Functional-First Simulation"。

**緩解**：
- 間接跳躍：退化處理 —— 預測錯時不走 shadow，而是「插入 K 個通用的 dummy uop」佔資源。這會低估 wrong-path 的 cache 影響但保留資源佔用效應。
- 先量化影響：用 gem5 跑一次「完全不模擬 wrong-path」與「正確模擬」的對照，看 IPC 差多少。若差 < 3%，這項風險可以接受。
- 接受限制並記錄在文件裡：BSTM 不適用於「推測執行行為本身是研究主題」的題目。

**偵測時機**：Phase 3。

---

## R4｜liveness 優化不足，工作集爆 L2【高 · 直接影響可行性】

**描述**：見 10.2。若每個 net 都佔一個 slot，26,000 個 net × 64 lane × 8 bytes = 13 MB，會掉出 L2。效能可能掉 5–10 倍，讓 BSTM 相對 Verilator 多進程沒有優勢。

**影響**：方案失去存在理由（見 10.3 的對照）。

**緩解**：
- bstm-cc 的 liveness + 圖著色是 **P0 功能**，不是優化項
- 目標：26,000 net → < 600 slot
- 排程時可以做 **cache-aware reordering**：把相依緊密的 cell 排在一起，縮短 net 生命期
- 若仍不夠，可把模型切成數個 phase，每個 phase 的工作集獨立

**偵測時機**：Phase 2，一做完 backend 就量。

---

## R5｜多埠結構在 Yosys 退化【高 · Phase 0 可預檢】

**描述**：見 6.5。ROB、wakeup CAM、RAT 這類多埠非同步讀取結構會退化成 FF + mux tree，造成閘數與邏輯深度爆炸。GEM 論文報告五個 benchmark 有四個踩到。

**影響**：cell 數可能膨脹 3–10 倍，效能等比例下降。

**緩解**：
- 建模時一律用同步讀取
- wakeup 改成 bit-matrix（在 bit-sliced 形式下反而便宜，見 6.5）
- Phase 0 先把 ROB/IQ/RAT 的草稿丟進 Yosys `memory_libmap` 預檢

**偵測時機**：Phase 0，一小時內可驗證。

---

## R6｜建模生產力太低，沒人願意用【中 · 長期】

**描述**：五條建模規則（第 5 章）加起來，建模生產力大約是 C++ 模型的 1/3 到 1/5。「不能用 `$display`」「不能有寬算術」「所有東西要最大尺寸 + mask」對習慣 C++ 的人是很大的負擔。

**緩解**：
- 提供一套 A-Port module library，把常用結構（queue、arbiter、scoreboard、counter）包好，使用者只做組裝
- Verilator 雙軌保留完整除錯能力 —— 要 debug 就跑 Verilator 版
- 提供 lint 工具，違反規則在寫的當下就報錯，而不是效能爛掉才發現

---

## R7｜準確度達不到 10% 門檻【中 · Phase 3】

**描述**：R2 + R3 疊加，加上模型本身的抽象誤差，可能讓 IPC 誤差超過業界門檻。

**緩解**：
- 只承諾**相對準確度**（趨勢正確），不承諾絕對準確度
- 對每個 workload 類別（compute-bound / memory-bound / branch-heavy）分別校準，記錄各自的系統性偏移
- 用 gem5 當持續 correlation 的基準

---

## R8｜SimPoint 對你的 workload 不適用【中】

**描述**：SimPoint 的誤差沒有統計保證，它是 clustering heuristic。對 SPEC 那種 phase 結構清楚的 workload 很好，對 OS-heavy / server / 高度非決定性的 workload 會失準。

**緩解**：
- SimPoint 當主力，留一兩個配置用全程跑或 SMARTS 校準
- 記錄每個 workload 的 SimPoint 覆蓋率與 BBV 分群品質

---

## R9｜專案規模被低估【中 · 常見】

**描述**：本計劃書描述的系統有三個子系統（前置、編譯、執行），每個都不小。原型（167 行）能跑不代表產品化容易。

**估計工作量**：

| 項目 | 人日 |
|---|---|
| bstm-cc（含 liveness、多 backend） | 25 |
| Spike `.bstf` writer + wrong-path | 15 |
| Cache 前置模擬 | 8 |
| Runtime（執行緒池、transpose、統計） | 12 |
| A-Port module library | 10 |
| 一個完整的 OOO timing model | 30 |
| 驗證基礎設施 | 12 |
| **合計** | **~112 人日** |

單人全職約 5–6 個月。**建議按 Phase 切，每個 Phase 都有獨立價值，隨時可以停。**

---

# 12. 分階段路線圖與決策閘

## 12.0 總覽

```
 Phase 0        Phase 1          Phase 2         Phase 3        Phase 4
 可行性          最小可行          規模化          準確度         擴展
 ────────       ────────         ────────        ────────       ────────
 1 週            3 週             6 週            8 週           待定
   │               │                │               │              │
   ▼               ▼                ▼               ▼              ▼
 ┌────┐         ┌────┐           ┌────┐          ┌────┐        ┌────┐
 │閘 0│────────►│閘 1│──────────►│閘 2│─────────►│閘 3│───────►│    │
 └────┘         └────┘           └────┘          └────┘        └────┘
 維度盤點       in-order 模型     OOO 模型        MLP + 校準     GPU/Metal
 Yosys 預檢     端到端跑通        liveness 優化   gem5 對照      多核 target
 DSE 需求       lane-0 對拍       效能達標        誤差 < 15%     發表 / 開源
```

## 12.1 Phase 0：可行性驗證（1 週）

**目的**：用最小成本確認這個方案對你的實際需求是否成立。**不寫任何產品程式碼。**

### 任務

**(a) DSE 維度盤點【最重要】**

列出你真正想掃的每一個維度，逐項標記：

```
 維度                     範圍          類型        可 bit-slice?
 ──────────────────────────────────────────────────────────────
 ROB 大小                 64~256        參數性      ✓（最大尺寸+mask）
 IQ 大小                  16~64         參數性      ✓
 LDQ/STQ 大小             16~48         參數性      ✓
 BPU 表格大小             4K~64K        參數性      ✓
 BPU 演算法               3 種          結構性      ✗ 三份編譯
 issue width              2/4/6         結構性      △ 可轉參數性
 ALU 數量                 2/3/4         結構性      △ 可轉參數性
 執行延遲表               多組          參數性      ✓
 bypass 網路拓樸          2 種          結構性      ✗
 ──────────────────────────────────────────────────────────────
```

**判準**：
- 參數性維度的組合數 ≥ 64 → **通過**
- 32 ~ 64 → 勉強，考慮把結構性轉參數性
- < 32 → **不通過**，直接用 Verilator 多進程

**(b) Yosys 多埠結構預檢**

把 ROB / IQ / RAT 的草稿寫法丟進 Yosys：

```tcl
read_verilog rob_draft.v
hierarchy -top rob
proc; opt; memory -nomap
memory_libmap -lib memlib.txt
stat
```

看是否出現 `$__RAMGEM_ASYNC_` 或 `using FF mapping for memory`。若大量出現，先改寫成同步讀取再重測。

**(c) wrong-path 影響量化**

用 gem5（或任何既有模型）跑一次「完全不模擬 wrong-path」vs「正確模擬」，看 IPC 差距。若 < 3%，R3 風險可降級。

### 決策閘 0

| 條件 | 結果 |
|---|---|
| 參數性維度組合 ≥ 64 **且** Yosys 預檢通過 | → Phase 1 |
| 參數性維度不足 | → 停止，改用 Verilator 多進程 |
| Yosys 預檢不通過但可改寫 | → 先改建模風格，重測 |

---

## 12.2 Phase 1：最小可行系統（3 週）

**目的**：端到端跑通一個完整但極簡的流程，建立所有介面契約。

### 任務

| # | 任務 | 產出 |
|---|---|---|
| 1.1 | 定義 `.bstf` 格式並凍結 | `bstf.h` |
| 1.2 | Spike 側 dump（先不做 wrong-path） | `spike-bstf` |
| 1.3 | 寫一個 5 級 in-order timing model（Verilog） | `model/inorder5/` |
| 1.4 | Verilator 參考模型跑通，對 Spike IPC | 參考 IPC 數字 |
| 1.5 | bstm-cc v0（原型升級：完整 cell 覆蓋、無 liveness） | `bstm-cc` |
| 1.6 | lane-0 對拍通過 | CI job |
| 1.7 | Runtime v0（單執行緒、64 lane、固定 batch） | `bstm-run` |
| 1.8 | 量測單核吞吐 | 效能報告 |

### 決策閘 1

- 端到端跑通、lane-0 bit-exact
- 單核吞吐 ≥ 5 M instance-cycles/s（in-order 模型，約 5,000 cell）
- 若吞吐嚴重低於預期，先找出原因再進 Phase 2

---

## 12.3 Phase 2：規模化（6 週）

**目的**：把系統推到真實 OOO 模型的規模，並確認效能仍然成立。

### 任務

| # | 任務 | 備註 |
|---|---|---|
| 2.1 | **bstm-cc liveness + slot 配置** | **P0，決定成敗** |
| 2.2 | bstm-cc 常數傳播與化簡 | |
| 2.3 | A-Port module library（queue/arbiter/scoreboard/counter） | 提升建模生產力 |
| 2.4 | OOO timing model（rename/IQ/ROB/LSU） | ~26,000 cell |
| 2.5 | wakeup 改成 bit-matrix 實作 | 對應 R5 |
| 2.6 | Runtime：執行緒池、work-stealing、bit transpose 優化 | |
| 2.7 | wrong-path shadow 產生與消費 | |
| 2.8 | SimPoint 整合（BBV → 區間選擇 → checkpoint） | |
| 2.9 | 統計收集與 counter 優化 | |

### 決策閘 2

| 指標 | 目標 |
|---|---|
| 工作集（slot 數 × 64 × 8） | < 512 KB |
| 單核吞吐 | ≥ 5 M instance-cycles/s（26,000 cell 模型） |
| 16 核吞吐 | ≥ 80 M instance-cycles/s |
| **vs Verilator 16 進程** | **≥ 2×，否則本方案無意義** |
| lane-0 對拍 | 100% 通過 |

最後一條是硬門檻。若 BSTM 打不贏「16 個 Verilator process」，應該誠實停止。

---

## 12.4 Phase 3：準確度（8 週）

**目的**：讓模型的數字可信。這是最長也最難的階段。

### 任務

| # | 任務 |
|---|---|
| 3.1 | 簡化 MLP 模型（MSHR + 未完成 miss 佇列）放回 timing model |
| 3.2 | Cache 前置模擬改成「MSHR 配到之後的延遲」語意 |
| 3.3 | branch predictor 模型校準（對真實 predictor 的 misprediction rate） |
| 3.4 | gem5 correlation：同一組 SimPoint、同一批 workload |
| 3.5 | 分類校準：compute / memory / branch-heavy 各自的系統性偏移 |
| 3.6 | 趨勢驗證：掃 ROB/IQ 大小，確認趨勢與 gem5 單調一致 |
| 3.7 | 若有 RTL：RTL correlation |

### 決策閘 3

| 指標 | 目標 |
|---|---|
| IPC 絕對誤差（vs gem5） | < 15% |
| 趨勢一致性 | 所有掃描維度單調方向相同，無交叉 |
| memory-bound workload 誤差 | < 20% |

---

## 12.5 Phase 4：擴展（待定）

在前三個 Phase 成功後才考慮：

- **Metal backend**：同一份 IR 出 Metal shader，利用 M5 Max 的 32/40 個 GPU core。預期 3–5× 於 CPU 版。unified memory 讓 trace 零複製共用 —— 這是 Apple 平台唯一實質勝過 discrete GPU 的地方。
- **CUDA backend**：同樣的資料佈局，移植成本低。
- **多核 target**：模擬多核系統（需要 coherence 模型，複雜度大增）。
- **開源與發表**：RISC-V 社群是天然受眾；Olympia 正在找 execution-driven 版本。

---

# 13. 程式碼組織與介面定義

## 13.1 目錄結構

```
bstm/
├── doc/
│   ├── PLAN.md                  ← 本文件
│   ├── modeling-guide.md        建模規範與 A-Port 使用說明
│   └── bstf-spec.md             trace 格式規格
│
├── tools/
│   ├── bstm-cc/                 編譯器：Yosys JSON → 目標程式碼
│   │   ├── src/parse.rs           JSON 解析
│   │   ├── src/graph.rs           net 相依圖、拓樸排序
│   │   ├── src/liveness.rs        ★ liveness + 圖著色 slot 配置
│   │   ├── src/lower.rs           cell → bitwise IR
│   │   ├── src/opt.rs             常數傳播、化簡
│   │   └── src/backend/
│   │       ├── c.rs                 純 C（uint64_t）
│   │       ├── neon.rs              ARM NEON（uint32x4_t）
│   │       ├── avx512.rs            x86 AVX-512
│   │       ├── metal.rs             Metal shader
│   │       └── cuda.rs              CUDA
│   │
│   ├── bstm-lint/               建模規範檢查
│   └── spike-bstf/              Spike 的 .bstf writer patch
│
├── lib/
│   ├── aport/                   A-Port module library（Verilog）
│   │   ├── aport.v                基本 port
│   │   ├── fifo.v                 可配置深度 FIFO
│   │   ├── arbiter.v              round-robin / priority
│   │   ├── scoreboard.v           bit-matrix 相依追蹤
│   │   ├── counter.v              bit-slice 友善的統計 counter
│   │   └── masked_array.v         最大尺寸 + runtime mask 的容器
│   │
│   └── runtime/
│       ├── bstf.h                 trace 格式定義
│       ├── transpose.c            64×64 bit transpose（含 NEON/AVX 特化）
│       ├── pool.c                 worker 執行緒池 + work stealing
│       ├── batch.c                instance 打包
│       └── stats.c                counter 抽取與聚合
│
├── model/
│   ├── inorder5/                Phase 1 的極簡模型
│   └── ooo/                     Phase 2 的完整 OOO 模型
│       ├── frontend/
│       ├── rename/
│       ├── backend/
│       ├── lsu/
│       └── top.v
│
├── preprocess/
│   ├── simpoint/                BBV 產生與區間選擇
│   └── cachesim/                記憶體前置模擬（多配置單次掃描）
│
├── test/
│   ├── equiv/                   Verilator ↔ bstm lane-0 對拍
│   ├── iss/                     ISS 一致性檢查
│   └── perf/                    效能迴歸
│
└── scripts/
    ├── build_model.sh           model.v → model_bs.c 的完整流程
    ├── sweep.py                 DSE sweep 驅動
    └── correlate.py             與 gem5 對照
```

## 13.2 關鍵介面契約

### 模型編譯介面

```bash
# 完整流程，一條命令
./scripts/build_model.sh model/ooo/top.v \
    --top ooo_top \
    --backend neon \
    --lanes 128 \
    --out build/ooo_neon.so

# 內部展開為：
#   yosys -p "read_verilog ...; hierarchy -top ooo_top; proc; opt_expr;
#             opt_clean; pmuxtree; flatten; opt -fast; write_json m.json"
#   bstm-cc m.json --backend neon --lanes 128 -o m.c
#   cc -O3 -shared -o ooo_neon.so m.c
```

### 執行期 ABI

編譯出來的模型必須提供以下符號：

```c
/* 模型的靜態描述 */
typedef struct {
    const char *name;
    uint32_t    lanes;          /* 64 / 128 / 512 */
    uint32_t    state_bytes;    /* sizeof(state_t) */
    uint32_t    n_counters;
    const char **counter_names;
    uint32_t    n_cfg_fields;
    const char **cfg_names;
    const uint8_t *cfg_widths;
} bstm_model_desc_t;

const bstm_model_desc_t *bstm_describe(void);

/* 初始化：每個 lane 不同的配置 */
void bstm_init(void *state, const uint32_t cfg[][MAX_CFG], uint32_t n_lanes);

/* 主迴圈：一個 target cycle */
void bstm_eval(void *next, const void *cur, const void *fetch_buf);

/* 詢問哪些 lane 需要補 fetch buffer（回傳 lane mask） */
uint64_t bstm_refill_mask(const void *state);

/* 抽取統計：counter[lane][idx] */
void bstm_extract(const void *state, uint64_t *counters, uint32_t n_lanes);

/* 結束判定 */
uint64_t bstm_done_mask(const void *state);
```

這個 ABI 的設計目標是讓 runtime 完全不需要知道模型內部結構，模型可以熱插拔。

### DSE 描述檔

```yaml
# sweep.yaml
model: build/ooo_neon.so
traces:
  - dir: traces/coremark/
    simpoints: [0, 1, 2, ..., 29]

sweep:
  rob_entries:   [64, 96, 128, 192, 256]
  iq_entries:    [16, 24, 32, 48]
  ldq_entries:   [16, 24, 32]
  bpu_table_log: [12, 14, 16]
  # 笛卡兒積 = 5 × 4 × 3 × 3 = 180 個配置

output:
  format: parquet
  path: results/sweep_001.parquet
  metrics: [ipc, mispred_rate, rob_occupancy, stall_breakdown]
```

## 13.3 技術選型

| 元件 | 選擇 | 理由 |
|---|---|---|
| bstm-cc 語言 | Rust | 大量圖演算法，需要效能與正確性；GEM 也用 Rust |
| 模型語言 | Verilog-2005 子集 | 沿用既有投資；Yosys 支援最好 |
| Runtime 語言 | C11 | 需要精確控制記憶體佈局與 intrinsics |
| 前置工具 | Python | 膠水層，不在關鍵路徑 |
| 結果格式 | Parquet | 上百萬列的 DSE 結果，需要欄式儲存 |
| 建置系統 | Make + Cargo | 簡單即可 |

---

# 14. 未來延伸方向

## 14.1 Metal / CUDA backend

同一份 bitwise IR 換一個 backend 即可。關鍵差異：

| | CPU | Metal | CUDA |
|---|---|---|---|
| word 型別 | `uint32x4_t` / `__m512i` | `uint4` | `uint4` |
| lane 數 | 128 / 512 | 128 | 128 |
| 平行單位 | thread | threadgroup | thread block |
| 全域同步 | 不需要 | **不需要** | 不需要 |
| trace 存取 | 直接 | **零複製（unified memory）** | 需 PCIe 搬運 |

**重要洞察**：Metal 沒有 grid-wide sync（無 cooperative groups 等價物），這讓移植 GEM 那類「把一個電路切開跨 threadgroup」的設計不可行。但 **BSTM 按 instance 切，threadgroup 之間完全獨立，根本不需要全域同步** —— Metal 最大的缺陷剛好被這個架構繞過。

Apple unified memory 讓 Spike 產生的 trace 對 GPU 零複製可見，這是 Apple 平台在此工作負載上唯一實質勝過 A100 的地方，而且它正好打在「ISS 橋接」這個關鍵路徑上。

**限制**：Apple GPU 的 threadgroup memory 是 32 KB。用 `uint32` bit-slice（32 instance）時，32 KB ÷ 4 bytes = 8,192 個 slot。這跟 liveness 優化的目標（< 600 slot）相比非常寬鬆，所以可行。

## 14.2 Tensor core / 矩陣單元

**結論：不建議，但值得知道為什麼。**

邏輯閘本來就是半環上的矩陣運算：GF(2) 上 AND = 乘法、XOR = 加法；Boolean 半環 (∨,∧) 可以用 `popcount(AND(a,b))` 取 threshold 實現。NVIDIA 有對應的硬體指令（BMMA，`bmmaBitOpAND` + popcount）。

但有兩個障礙：

1. **Arithmetic intensity。** tensor core 需要 O(N³) 運算攤在 O(N²) 資料上。電路求值是 matrix-**vector**（一個路由矩陣套用在一個 state vector 上），intensity 是 O(1)。**只有加上 instance 維度變成 matrix-matrix 才划算** —— 又一次指向同一個結論。
2. **硬體正在消失。** CUDA 文件：b1 with `bmmaBitOpXOR` 在 sm_90 被移除；Hopper 上 AND 還在但 XOR 極慢；Blackwell 狀態 NVIDIA 未回覆。Apple 從 SME 到 Metal tensor ops 全部最低只到 INT8，**沒有任何 1-bit 路徑**。

**唯一值得考慮的落點**：把 OOO 模型裡天生 all-pairs 的結構（wakeup matrix、age matrix、crossbar allocation）丟給矩陣單元，其餘留在 bitwise ALU。因為這些結構在閘級是**深**的（priority encoder + 寬 mux 樹），一次 MMA 取代一棵深樹有實質好處。但這是 Phase 5 以後的事。

## 14.3 回到 timing-directed

若某天需要精確的 wrong-path（R3 無法接受），可以考慮混合模式：

- 大部分時間跑 functional-first（快）
- 遇到無法預先展開的情況（間接跳躍 mispredict）時，暫停該 lane，回 host 呼叫 Spike

這會嚴重拖慢，但可以當成「校準模式」—— 用少數幾個配置跑精確版，量出 functional-first 的系統性偏移，再把偏移套用到大規模 sweep 的結果上。這是一個務實的折衷。

## 14.4 逐段 refine 成 RTL

這是 Verilog 當建模語言最獨特的長期價值，也是 C++ 模型永遠做不到的：

```
   Phase A:  全部是 timing model（抽象）
   Phase B:  fetch/decode 換成真 RTL，其餘仍是 model
   Phase C:  加上 rename 的真 RTL
   ...
   Phase Z:  全部是 RTL
```

每一步都可以用同一套測試向量驗證「換掉的部分時序行為不變」。correlate 時不需要重寫任何東西，因為模型和 RTL 用的是同一種語言、同一種語意。

這條路線讓效能模型與 RTL 實作之間不再是兩個分裂的世界，而是同一條連續的光譜。如果 BSTM 最後只留下一個貢獻，我認為會是這個。

---

# 15. 附錄

## 15.1 原型實測資料

**環境**：Intel i7-12700、Linux 7.0.0-30、gcc `-O3 -march=native`、Yosys 0.36+42（docker `hdlc/yosys`）、Verilator 5.020

**原始檔案位置**：`~/work/yosys-demo/`

| 檔案 | 說明 |
|---|---|
| `stage.v` | 53 行的 timing model 片段 |
| `chain.v` | 32 級串接（830 cell） |
| `stage_wide.v` / `chainw.v` | 16-bit 欄位變體 |
| `gen.py` | 167 行的 bit-sliced C 產生器 |
| `stage_bitsliced.c` / `chain_bs.c` | 產生的 C |
| `bench.c` / `cbench.c` / `wbench.c` | BSTM benchmark |
| `vbench*.cpp` | Verilator benchmark |

**完整結果**：

```
=== 25 cell（單 stage） ===
Verilator  : 27.6 M target-cycles/s
bit-sliced : 164.0 M target-cycles/s  |  10,497 M instance-cycles/s

=== 830 cell，4-bit 欄位 ===
Verilator  :  5.99 M target-cycles/s
bit-sliced :  3.24 M target-cycles/s  |    208 M instance-cycles/s

=== 830 cell，16-bit 欄位 ===
Verilator  :  5.91 M target-cycles/s
bit-sliced :  1.07 M target-cycles/s  |     69 M instance-cycles/s
```

**重現命令**：

```bash
cd ~/work/yosys-demo

# Yosys 合成
docker run --rm -v "$PWD":/w -w /w hdlc/yosys:latest yosys -p '
  read_verilog stage.v chain.v
  hierarchy -check -top chain
  proc; opt_expr; opt_clean; flatten; opt -fast
  write_json chain.json; stat'

# 產生 bit-sliced C
python3 gen.py chain.json chain > chain_bs.c

# Benchmark
gcc -O3 -march=native -o cbench cbench.c && ./cbench
verilator --cc --build --exe -CFLAGS "-O3 -march=native" \
  --top-module chain stage.v chain.v vbench2.cpp -o vbench2 --Mdir obj_v2
./obj_v2/vbench2
```

## 15.2 參考文獻

**Decoupled simulation 理論**
- Mauer, Hill, Wood. *Full-system timing-first simulation.* SIGMETRICS 2002.
- Martin et al. *Multifacet's General Execution-driven Multiprocessor Simulator (GEMS).* CAN 2005.

**Timing model 抽象**
- Pellauer et al. *A-Port Networks: Preserving the Timed Behavior of Synchronous Systems for Modeling on FPGAs.* ACM TRETS 2:3, 2009.
- Pellauer, Adler, Kinsy, Parashar, Emer. *HAsim: FPGA-based high-detail multicore simulation using time-division multiplexing.* HPCA 2011.
- Vachharajani et al. *The Liberty Simulation Environment, Version 1.0.* 2004.
- August et al. *UNISIM: An Open Simulation Environment and Library for Complex Architecture Design.* CAL 2007.

**FPGA 加速模擬**
- Chiou et al. *FPGA-Accelerated Simulation Technologies (FAST).* MICRO 2007.
- Tan et al. *RAMP Gold: An FPGA-based Architecture Simulator for Multiprocessors.* DAC 2010.
- Karandikar et al. *FireSim: FPGA-Accelerated Cycle-Exact Scale-Out System Simulation.* ISCA 2018.
- Biancolin et al. *Golden Gate: Bridging The Resource-Efficiency Gap.* ICCAD 2019.
- Kim et al. *Strober: Fast and Accurate Sample-Based Energy Simulation for Arbitrary RTL.* ISCA 2016.

**取樣**
- Sherwood et al. *Automatically Characterizing Large Scale Program Behavior (SimPoint).* ASPLOS 2002.
- Wunderlich et al. *SMARTS: Accelerating Microarchitecture Simulation via Rigorous Statistical Sampling.* ISCA 2003.

**GPU / 平行 RTL 模擬**
- Guo, Zhang, Wang, Lin, Ren. *GEM: GPU-Accelerated Emulator-Inspired RTL Simulation.* DAC 2025.
- Lin, Ren, Zhang, Huang. *From RTL to CUDA: A GPU Acceleration Flow for RTL Simulation with Batch Stimulus.* ICPP 2022.
- Emami et al. *Parendi: Thousand-Way Parallel RTL Simulation.* ASPLOS 2025.
- Wang, Beamer. *RepCut: Superlinear Parallel RTL Simulation with Replication-Aided Partitioning.* ASPLOS 2023.

**Bit-parallel 模擬**
- Waicukauski et al. *Fault simulation for structured VLSI (PPSFP).* VLSI Systems Design 1985.
- Yosys / CXXRTL 文件。

**效能模型驗證**
- Butko et al. *Accuracy evaluation of GEM5 simulator system.* ReCoSoC 2012.
- Gutierrez et al. *Sources of error in full-system simulation.* ISPASS 2014.
- Akram, Sawalha. *A Survey of Computer Architecture Simulation Techniques and Tools.* IEEE Access 2019.

**現代 RISC-V 效能模型**
- riscv-software-src/riscv-perf-model（Olympia）
- Bosch et al. *gem5+RTL: A Framework to Enable RTL Models Inside a Full-System Simulator.* ICPP 2021.

## 15.3 名詞對照

| 英文 | 中文 | 說明 |
|---|---|---|
| bit-slicing | 位元切片 | 把 N 個 instance 的同一條訊號打包進一個機器字 |
| lane | 通道 | 一個 word 裡的一個 bit 位置，對應一個 instance |
| instance | 實例 | 一組 (config, simpoint) 的組合 |
| target cycle | 目標週期 | 被模擬的 CPU 的一個時脈週期 |
| host | 宿主 | 跑模擬的那台機器 |
| oblivious full-cycle | 無差別全週期 | 每 cycle 求值整個電路，不做事件剪枝 |
| A-Port | A 埠 | 固定延遲、固定深度的模組間通訊介面 |
| functional-first | 功能優先 | ISS 先跑產生 trace，timing model 事後消費 |
| timing-directed | 時序主導 | timing model 指揮 functional model 執行 |
| wrong-path | 錯誤路徑 | 分支預測錯誤後被推測執行的指令 |
| MLP | 記憶體階層平行度 | memory-level parallelism |
| DSE | 設計空間探索 | design space exploration |

## 15.4 決策紀錄（ADR 摘要）

| # | 決策 | 理由 | 可逆性 |
|---|---|---|---|
| 1 | 採用 functional-first 而非 timing-directed | 只有 functional-first 能 bit-slice | 低（影響全域架構） |
| 2 | 保留 Verilog 而非改用 C++ EDSL | 沿用既有投資；語意天然 branchless；可 refine 成 RTL | 中 |
| 3 | 停在 word-level RTLIL 而非 AIG gate | 保留 `$add`/`$eq` 結構；避免閘級爆炸與可觀測性喪失 | 高 |
| 4 | 記憶體延遲前置解析 | 移除模型裡唯一不適合 bit-slice 的部分 | 中（Phase 3 部分回收） |
| 5 | CPU 優先，GPU 延後 | 除錯能力；資料佈局同構，移植成本低 | 高 |
| 6 | 最大尺寸 + runtime mask | 讓尺寸維度變成可 bit-slice 的參數 | **極低**（必須一開始就決定） |
| 7 | 定位為 DSE 加速器而非效能模型替代品 | 可見度弱勢無法克服；雙軌並行成本為零 | 高 |

---

---

# 16. 實作回饋：計劃書被自己的實作推翻的地方

本章於 2026-09-21 完成 demo 實作後追加。**這些是整個專案最有價值的產出** ——
計劃書寫對的地方只是沒錯，寫錯的地方才是新知識。

## 16.1 七處被實測推翻

| § | 計劃書的說法 | 實測 | 影響 |
|---|---|---|---|
| 10.2 | 模型約 26,000 cells | **38,549** | 高 48%，同量級，估算法可用 |
| 10.2 | 工作集目標 < 512 KB | **104 KB**（27 KB slot + 77 KB state） | **達標，餘裕 5 倍** |
| 8.5 | `REFILL_M` 決定攤銷倍率 | **錯**。8→128 掃描幾乎是平的（750→659 ns） | 瓶頸是「每筆記錄的解碼」，攤銷不掉。應離線預解碼成定長 entry |
| 10.2 | cell 數可當成本代理 | **錯**。gate-level 差到 **11.9×**，排序完全相反 | `be_iq` cells 7,852 / gates 93,707；`lsu_q` cells 12,743 / gates 27,949。成本一律用 `ci/gate_count.sh` |
| 11 R2 | MLP 風險「嚴重」 | MSHR 模型僅佔 **1.8%** 成本 | 下修，並從 Phase 3 提前到 Phase 1 |
| 11 R3 | D 的時序耦合「量一下不大就固定」 | **錯**。有懸崖，`D == ubtb_entries` 時 uBTB 命中率歸零 | D 必須當掃描維度，且要避開 `D ≈ 預測器容量` |
| 6.4 | liveness 是唯一的 P0 | 做到了（40.8×），但**指令足跡成為新瓶頸** | 見 16.2 |

## 16.2 計劃書完全沒預見的：指令足跡

liveness 把**資料**工作集從 139,924 net 壓到 3,432 slot（27 KB）之後，瓶頸換成了**程式碼大小**：

```
141,317 個 bitwise op  ≈  800 KB 機器碼
Alder Lake L1i         =   32 KB
```

實測 20,015-cell 的設計比線性外推**慢約 1.7 倍**，就是 L1i miss。

這改變了下一步的優先序。可能的方向：迴圈化重複結構、op fusion 降低 op 數、
或改用更緊湊的編碼（見 16.3）。

## 16.3 Backend 的四方實測（141,317 ops/cycle 真實模型）

| backend | 編譯時間 | target-cycles/s | ops/s |
|---|---|---|---|
| 單一大函式 + gcc -O1 | **10 分鐘後 OOM（27 GB）** | — | — |
| bytecode interpreter | 0.02 s | 4.8 K | 0.68 G |
| 手工 emit x86-64 JIT | **0.003 s** | 11.8 K | 1.66 G |
| **切 36 函式 + gcc -O2** | 4 分 47 秒 | **36.7 K** | **5.19 G** |

三個結論：

1. **「emit C 給 gcc」在真實規模上會直接失敗。** 必須切函式（Verilator `--output-split` 的同一個手法）。切了之後 `-O1` 從 832 s 降到 110 s，執行速度不變。
2. **「register allocation 沒東西可做」是錯的。** 手工 JIT 每個 op 都 load-load-op-store（28 B/op），gcc 靠把區域鏈留在暫存器贏了 **3.1 倍**。
3. **JIT 的價值不是速度，是編譯時間**（0.003 s vs 287 s，差 95,000 倍）。掃 100 個結構性配置：gcc 要 8 小時，JIT 要 0.3 秒。正解是兩個 backend 並存。

**現成的替代品**：CIRCT 的 **arcilator** 已經在做「電路 → LLVM IR → JIT」，有專門的 `arc` dialect。
它不做 bit-slicing，但那在 MLIR 裡只是一個 `i1` → `i64` 的型別轉換 pass。
比自己刻 JIT 好得多的起點。

## 16.4 度量歸因：同一類錯誤發作四次

這是整個 demo 最重要的發現，值得單獨一節。

### 四次發作

| # | 抓到的人 | 錯誤 | 錯誤導致的結論 |
|---|---|---|---|
| 1 | Agent F | `proc;opt` 的 cell 數當成本代理 | gate 差 11.9×，模組成本**排序完全相反** |
| 2 | 監督者 | 工作集乘了兩次 64 | 6.5 MB vs 27 KB，**可行性判斷相反** |
| 3 | Agent E | `gate_count.sh` 把 yosys 兩段 stat 都加總 | 所有 gate/flop 數 **2 倍** |
| 4 | **PRF sweep 本身** | `cnt_st_rename` 吃下所有下游回壓 | **指向錯誤的瓶頸** |
| 4b | Agent E | `cnt_st_iq` 與 `cnt_st_lsq` 定義不對等 | 灌水的 counter 反而比較小 |
| 4c | Agent D | `cnt_st_fetch` 吃下誤預測後的重填拍 | 前端斷流被高估 |

**六次全部是度量出錯而不是邏輯出錯，而且每一次的結論都是反的。**

### 第 4 次特別值得記：它是被實驗逼出來的

前三次是人看出來的。第 4 次不是 —— 我在 README 寫下「rename 佔 51% stall、PRF 64 配 ROB 64 嚴重失衡，這是配置結論不是 bug」，還特別強調了它的地位。

**只有跑了 sweep 才發現它是錯的**：

```
PRF     IPC      rename(修正前)   rename(修正後)
 64    1.381       254,365          61,457
 96    1.391       245,580           2,936
128    1.391       245,580               0
256    1.391       245,580               0
```

修正前「stall 停在 245,580 不降」，看起來像 PRF 加大無效。修正後才看得出**真正的 freelist stall 在 PRF 128 精確歸零** —— 與 Agent E 事前從第一性原理推導的飽和點（32 已 commit 映射 + ≤64 in-flight = 96）一致。

如果沒跑這個 sweep，那個錯誤結論會一直留在 README 上。

### 修正後浮現的真實圖像

```
PRF  64 → 128:  rename stall  61,457 → 0
                rob    stall     141 → 40,445   （增加 287 倍）
                ROB 佔用       28.53 → 34.14
                IPC            1.381 → 1.391   （+0.7%）
```

**Amdahl 的教科書演示**：修掉 rename 之後 ROB 立刻成為第二瓶頸，而 IPC 幾乎不動，因為 LSQ（122,202）和 IQ（85,243）在後面等著。

### 由此得到的三條規則（應寫進任何類似專案的契約）

1. **每個 stall cycle 恰好歸因到一個來源，且必須是真正的來源。**
   下游回壓不可記在上游頭上。重構 counter 時要能證明**新舊集合的和相等**（Agent D 的做法），而不是「看起來合理」。
2. **counter 的定義必須對等。** `cnt_st_lsq` 只在真的有 uop 被擋時計數，`cnt_st_iq` 卻不管有沒有東西要送都算 —— 兩者不可直接比較。
3. **斷言要用「精確等於」而不是「≥」。** 只有 `cnt == +5` 這種斷言抓得到重複計數，`>= 5` 永遠會過。

## 16.5 兩個 sweep 方法論的陷阱

### (a) 成本是量化的，不跟 `cfg_*` 走

建模規則 2 要求「最大尺寸 + runtime mask」，所以 **`cfg_iq_entries=48` 的 gate/state 成本與 64 完全相同**（結構要建到 `IQ_W=6`）。同理 PRF 96 的成本 = PRF 128 的成本。

**IQ 真正能選的只有 32 或 64，LDQ/STQ 只有 16 或 32。48 這種點位只有 IPC 意義、沒有成本意義。**

畫成本/效益曲線時，橫軸必須是**量化後的成本點**而不是配置值。

### (b) `full` 訊號可能是協定假象而非容量不足

`cnt_st_lsq` 報 LSQ 滿佔 22.4% 的 cycle。但用 Little's Law 反推：

```
記憶體 uop 配置率 = IPC 1.381 × 記憶體佔比 28.0% = 0.387 /cycle
ROB 平均停留      = 佔用 28.53 ÷ IPC = 20.7 cycles
→ LSQ 平均佔用   ≈ 8.0 / 32 entry = 25%
```

**平均只用掉 25% 的容量，卻有 22% 的時間說「滿」。**

原因是 all-or-nothing 的 dispatch 協定（CONTRACT §6）：「剩不到 `W`=4 格」就等於「滿」。**加 LDQ/STQ entry 不會有用，該改的是協定。**

進場花錢之前的三個零成本檢查（Agent E 提出）：
1. 先看 `cnt_st_mshr` —— 若 `lsq_full` 是 MSHR 耗盡的症狀，加 entry 無效（本例實測恆為 0，排除）
2. 加佔用直方圖 —— 若平均佔用遠低於容量，就是門檻假象
3. 把 `full` 的定義暫時改成「真的 0 格」再跑一次，差額就是協定造成的假 stall

## 16.6 一個值得記住的失敗：單位錯誤

監督者在中途把工作集算成 `3,432 slot × 64 lane × 8 B = 6.5 MB`，據此宣布
「塞不進 L2，PLAN §10.3 的懸念成立、方案可能不划算」。

**正確值是 `3,432 × 8 B = 27 KB`** —— 一個 `vec_t` 本身就是 64 個 lane，不需要再乘一次。

差 64 倍，而且結論完全相反（「不成立」→「餘裕 5 倍」）。

這與 Agent F 發現的 cell-vs-gate 度量問題是同一類錯誤，在 30 個缺陷裡已是第二次。
**度量單位錯一次，整個判斷就翻面。** 任何效能數字都應該附上推導過程，而不是只報結果。

## 結語

這個計劃的核心論點可以用一句話概括：

> **微架構效能探索的瓶頸不在單次模擬的速度，而在於沒有人把「同時模擬數千個獨立配置」這個維度拿出來用。**

整份文件從五個不同方向（A-Port 抽象、Yosys 編譯鏈、ISS 解耦、記憶體容量、GPU 移植路徑）分析下來，每一次都收斂到同一個結論。這個收斂性本身就是這個想法值得做的最好證據。

但同樣重要的是誠實面對它的邊界：BSTM 在單一 instance 上比 Verilator 慢，失去了 MLP 建模能力，wrong-path 處理不完整，建模生產力較低。它不是效能模型的替代品，而是一個**特化於粗掃的加速器**，需要跟可讀的 Verilator 版本雙軌並行。

第 12.1 節的 DSE 維度盤點是第一道也是最重要的一道閘。它花一小時，卻決定整個專案有沒有意義。**在寫任何一行產品程式碼之前，先做完它。**
