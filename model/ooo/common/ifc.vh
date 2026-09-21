// ===================================================================
// BSTM OOO model — 共用介面定義        契約檔，agent 不可改
// 所有欄位刻意壓窄（建模規則 1）。PC/位址/資料不存在於本模型。
// ===================================================================
`ifndef BSTM_IFC_VH
`define BSTM_IFC_VH

`define W          4          // fetch/issue/commit width
`define ROB_N     64
`define ROB_W      6
`define IQ_N      32
`define IQ_W       5
`define LDQ_N     16
`define STQ_N     16
`define LSQ_W      4
`define MSHR_N     8
`define MSHR_W     3
`define PRF_N     64
`define PRF_W      6
`define ARF_W      6          // 架構暫存器 0..63 (int+fp 合併編號)
`define UC_W       4          // uop class
`define LAT_W      4          // 執行延遲

// uop class（與 include/bstf.h 的 enum 一致）
`define UC_ALU    4'd0
`define UC_MUL    4'd1
`define UC_DIV    4'd2
`define UC_FPU    4'd3
`define UC_LOAD   4'd4
`define UC_STORE  4'd5
`define UC_BRANCH 4'd6
`define UC_JUMP   4'd7
`define UC_JALR   4'd8
`define UC_RET    4'd9
`define UC_CSR    4'd10
`define UC_FENCE  4'd11
`define UC_AMO    4'd12
`define UC_NOP    4'd13
`define UC_SYS    4'd14
`define UC_VEC    4'd15

// ---- 解碼後的 uop（rename 前），每條 26 bit ----
//  [25]    wrongpath
//  [24]    blk_end
//  [23:20] uop_class
//  [19:16] exec_lat
//  [15:10] src1  (bit15 = valid)
//  [ 9: 4] src2  (bit9  = valid)
//  [ 3: 0] dst 的低 4 bit ... 不夠，改用結構化巨集：
`define DUOP_W  32
`define DUOP_WRONGPATH  31
`define DUOP_BLKEND     30
`define DUOP_SERIALIZE  29
`define DUOP_MEMSTORE   28
`define DUOP_CLASS      27:24
`define DUOP_LAT        23:20
`define DUOP_S1V        19
`define DUOP_S1         18:13
`define DUOP_S2V        12
`define DUOP_S2         11:6
`define DUOP_DV         5
`define DUOP_D          4:0      // 架構暫存器低 5 bit（demo 只用 x0..x31）

// ---- rename 後的 uop ----
`define RUOP_W  40          // v3: 32->40，高 8 bit 新增架構暫存器編號
`define RUOP_WRONGPATH  31
`define RUOP_CLASS      30:27
`define RUOP_LAT        26:23
`define RUOP_S1V        22
`define RUOP_S1         21:16     // phys reg
`define RUOP_S2V        15
`define RUOP_S2         14:9
`define RUOP_DV         8
`define RUOP_D          7:2       // phys reg
`define RUOP_MEMSTORE   1
`define RUOP_SERIALIZE  0
// ---- v3 新增（bit[31:0] 佈局完全不變，只往高位長）----
`define RUOP_ARFD       36:32     // 架構目的暫存器 x0..x31，供 ROB 產生 cmt_arf
`define RUOP_ARFDV      37        // 架構目的暫存器 valid
// bit 38..39 保留


// ---- commit 介面語意（v2，2026-09-21 監督者裁決）----
//  cmt_prf 帶的是「這條 commit 的 uop 寫入的實體暫存器（新 mapping）」。
//  舊 mapping 由 rename 端的 committed-RAT 自行查出並釋放：
//      old = cRAT[cmt_arf]; free(old); cRAT[cmt_arf] = cmt_prf;
//  cRAT 同時是 flush 復原的正確來源（優於 identity reset）。
//
// ---- ready 語意（v2）----
//  所有 *_ready 都是單 bit，語意為 all-or-nothing：
//  ready=0 時該拍一條都不可送出，不支援 partial accept。
`endif
