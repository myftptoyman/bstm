# Agent D — 前段模型（`fe_front` + `rn_rename`）

| 檔案 | 模組 | 角色 |
|---|---|---|
| `frontend/fe_front.v` | `fe_front` | fetch + decode queue + redirect |
| `rename/rn_rename.v`  | `rn_rename` | RAT + committed-RAT + freelist，4-wide rename |

埠列由監督者凍結，本次只填 body。語言 Verilog-2005，遵守 `CONTRACT.md §1` 五條硬規則。
介面版本：**ifc.vh v3**（`RUOP_W` 40 bit，新增 `RUOP_ARFD/ARFDV`；commit 送新 mapping
`cmt_prf`；ready 為 all-or-nothing）。

---

## 1. 驗收結果

`ci/check_contract.sh model/ooo/frontend/fe_front.v model/ooo/rename/rn_rename.v`
→ **PASS=10 FAIL=0**

| top | cells | memories | latch | state bits |
|---|---|---|---|---|
| `fe_front`  | 449  | **0** | 無 | 617（dq 512 + 控制 9 + counter 96）|
| `rn_rename` | 4761 | **0** | 無 | 560（RAT 192 + cRAT 192 + fl 64 + cfl 64 + counter 48）|

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

### 2.4 counter 語意

| 埠 | 定義 |
|---|---|
| `cnt_st_fetch` | `de_ready & (de_valid == 0) & ~flush` 的 cycle 數：**下游收得下、但前端一條 uop 都交不出來**的拍數（含 I-cache 泡泡、斷流、flush 後重填）。**不含**「交得出但不足 4 條」的部分斷流。 |
| `cnt_mispred` | 前端偵測到的方向誤預測次數（= 切 shadow 的次數）。監督者裁定誤預測計數歸前端，`be_rob` 不重複計。 |

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

### 3.6 counter 語意

| 埠 | 定義 |
|---|---|
| `cnt_st_rename` | `(de_valid != 0) & (~rn_ready | ~enough_free) & ~flush` 的 cycle 數。**同時含 freelist 耗盡與下游 backpressure 兩種 stall**（契約指定）。要分離兩者需新增埠。 |

---

## 4. 已知限制與簡化

1. **架構暫存器只用 x0..x31**（`ARF_W` 6 bit 但 `DUOP_D` 只有 5 bit）。FP 不進 trace，
   沒有獨立的 FP RAT。`DUOP_S1/S2` 的 bit5 只參與相依比對，RAT 索引取低 5 bit；
   cRAT 則用完整 6-bit 比對（arch ≥ 32 不會命中）。監督者已裁定為已知限制。
2. **一拍不跨 fetch block 取 uop**（§2.2）。block 很短時會低估 fetch 頻寬，
   但這是 `fb_fe_event` per-block 語意的必然結果。
3. **decode queue 深度固定 16**，沒有對應的 `cfg_*`（契約 §3 沒給前端 queue 的配置暫存器），
   所以這一維無法做 DSE。監督者已接受為已知限制。
4. **`cnt_st_fetch` 只計「完全交不出 uop」**，不計「交得出但不足 4 條」。
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

§3.4 的相依已由監督者確認並寫進契約：`be_rob` 是 commit-time flush（走到 wrong-path uop
才拉 flush 並丟掉 ROB 其餘全部項目），所以 **cRAT 復原是精確的，不是近似**。
未來若有人把 `be_rob` 改成 ROB 中途 flush，rename 必須同步改成 checkpoint / ROB walk，
否則會把仍在 in-flight 的老 uop 的實體暫存器誤放回 freelist（已有回歸檢查把關）。
