# test/（Agent G）

```
unit/      runtime 的單元測試（含與 scalar 參考實作的逐筆對照）
bench/     transpose / refill 的效能數字
equiv/     ★ Verilator <-> bit-sliced 逐 cycle 對拍（PLAN §9.2 第一層驗證）
mock/      假的 ooo_top（Verilog + 手寫 bit-sliced），讓 runtime 在真模型到位前能跑
tools/     mkbstf：合成 .bstf / .fe / .mem / .fe.wp / .mem.wp / .meta.json
traces/    mkbstf 產生的測試 trace（不進 repo 的 traces/，那是 Agent A/B 的）
```

```sh
make test          # 8 支單元測試
make bench         # transpose / refill benchmark
make equiv         # 或 ./equiv/run_equiv.sh [N]
make check-real    # 用 Agent H 的真 trace 做整合檢查（REAL=... 可換）
```

工具：`build/mkbstf`（合成 trace）、`build/bstfdump`（檢查一份 .bstf + overlay
能不能被 runtime 正確讀；整合時先跑這支）。

## 單元測試

| | 檢查什麼 |
|---|---|
| `t_transpose` | 4096 個逐 bit 的轉置關係、自反性、pack/unpack 往返、小 n 特化 vs 完整轉置 |
| `t_bstf` | overlay 大小檢查、`.meta.json`、`idx(0)==0`（`delta(0)` 不可加）、每筆 LOAD/STORE/AMO 的 `mem_idx` 連續遞增且總數 == `n_mem_access`、非 mem 記錄的 mem event 必須是 0、`seek_index` vs 逐筆推進、shadow 進出與續行點、duop 欄位打包 |
| `t_refill` | 3000 cycle × 64 lane，**每 lane 每 cycle 的 window 內容**與 scalar 參考游標逐欄位比對 |
| `t_refill_wp` | **STARVED**：shadow 供完但 flush 沒到時停止供應、游標不動、50 拍後仍不溜回正確路徑，flush 後第一筆剛好是分支的下一筆 |
| `t_shadow` | **wrong-path 端到端**：跑完整條 trace，驗證 `cp_consumed == n_records`（恰好交付一次）、進出配對、`cnt_wrongpath == wp_consumed`、`cnt_mispred == enter+nobranch+resolved`、`cnt_st_refill > 0`；附負測證明單調性斷言會 abort |
| `t_batch` | 打包策略（同 simpoint 同 batch）、空 lane、cfg 複製、所有 instance 都排到 |
| `t_stats` | bit-sliced 48-bit counter 的 inject/extract 往返（64 lane × 11 counter） |
| `t_pool` | 200 個不等量 batch / 8 worker：每個剛好跑一次、聚合正確、真的有平行、pool 可重用 |
| `t_end2end` | mmap → 打包 → thread pool → mock 模型 → CSV，並檢查每 lane 統計合理 |

## mock 模型

`mock/mock_ooo_top.v` 的**埠列完全照 `model/ooo/top.v`**（v2 的兩處變更都在內部
wire，對外埠列沒動，已確認一致）。行為刻意極簡：每拍吃
`min(有效筆數, cfg_fetch_width)`、ROB 滿就吃 0、retire `min(occ, cfg_commit_width)`、
`FE_REDIRECT & ~FE_DIR_OK` 就跳 shadow 12 拍。11 個 48-bit counter 與 top.v 同名。

wrong-path 的部分刻意做成**可驗證**的：
- `occ` 記全部 in-flight、`occ_cp` 只記 correct-path，`retire = min(occ_cp, cw)`
  → **wrong-path uop 佔資源但永遠不會 retire**
- flush（`sh_end`）到達時 `occ <= occ_cp`，wrong-path 整批丟掉
- `cnt_wrongpath` 逐 slot 數 `fb_duop` 的 `DUOP_WRONGPATH` 位元（不是看模型自己的
  `in_shadow`）→ 這個 counter 直接驗證 runtime 真的標了旗標，而且數字必須**精確等於**
  runtime 供應的 wrong-path 記錄數
- `cnt_st_fetch` 拆成 `cnt_st_fetch` + `cnt_st_refill`，兩者互斥、相加等於原值
  （CONTRACT §8 規則 2）

`mock/mock_ooo_bs.c` 是它的**手寫 bit-sliced 版本**（將來應該由 `tools/bstm-cc`
從 Verilog 產生）。兩者每 cycle bit-exact —— 見 `equiv/`。
