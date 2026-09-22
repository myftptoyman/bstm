`include "common/ifc.vh"
// ===================================================================
// Agent D — rn_rename : RAT + freelist，4-wide           Verilog-2005
// 埠列已凍結，不可增刪。建模硬規則見 CONTRACT.md §1。
//   - 推測 RAT 與 committed-RAT(cRAT) 各 32 x 6 bit，攤平成向量；
//     讀走 mux 鏈、寫走展開的 per-entry 比較器，完全沒有變數索引的
//     記憶體讀 -> 不會出現 $mem/RAMGEM_ASYNC
//   - commit 介面 v2：ROB 送新 mapping(cmt_prf)，舊 mapping 由 cRAT 查出
//         old = cRAT[cmt_arf]; free(old); cRAT[cmt_arf] = cmt_prf;
//   - flush：RAT <- cRAT，freelist <- committed freelist(cfl)
//   - freelist 用 64-bit bitmask + 4 級 priority pick（純邏輯，無算術）
//   - 48-bit counter 用 6 段 8-bit 進位鏈（規則 1：無寬算術）
//   - 實體暫存器的寬度/數量全部走 `PRF_W / `PRF_N，沒有硬編 6/64：
//     PRF sweep（64/128/192/256）只要改 ifc.vh 就能重編，不必動本檔
// ===================================================================
module rn_rename (
    input  wire clk, input wire rst, input wire flush,
    input  wire [`W-1:0]         de_valid,
    input  wire [`W*`DUOP_W-1:0] de_duop,
    output wire [2:0]            de_nready,    // v8: 我這拍能收前 N 條（0..`W）
    output wire [`W-1:0]         rn_valid,
    output wire [`W*`RUOP_W-1:0] rn_ruop,
    input  wire [2:0]            rn_nready,    // v8: 下游這拍能收前 N 條
    input  wire [`W-1:0]         cmt_valid,
    input  wire [`W-1:0]         cmt_dv,
    input  wire [`W*`ARF_W-1:0]  cmt_arf,
    input  wire [`W*`PRF_W-1:0]  cmt_prf,      // v2: 新 mapping（非舊的）
    output wire [47:0]           cnt_st_rename,
    output wire [47:0]           cnt_st_backpressure,
    output wire [47:0]           cnt_lost_rename,
    output wire [47:0]           cnt_lost_backpressure
);

localparam ARF_N  = 32;          // 架構暫存器數，demo 只用 x0..x31
// ---- 下面三個常數「碰巧」是 5/4/6，但語意與 `PRF_W / `ROB_W 完全無關，
//      做 PRF/ROB sweep 時不可以跟著改（也不要被 regex 一把掃掉）----
localparam ARFI_W = 5;           // 架構暫存器索引寬度 = DUOP_D 寬度（ARF_N=32）
localparam CNT_B  = 6;           // 48-bit counter 切成 6 段 8-bit 進位鏈
localparam CW     = CNT_B * 8;   // = 48，counter 埠寬
localparam TCW    = $clog2(`W) + 1;   // 條數計數寬度（0..`W 都要裝得下）
localparam DW     = `DUOP_W;
localparam RW     = `RUOP_W;

// ---------------- 狀態 ----------------
reg [ARF_N*`PRF_W-1:0] rat_q;    // 推測 RAT：架構暫存器 -> 實體暫存器
reg [`PRF_N-1:0]       fl_q;     // 推測 freelist bitmask，1 = 可配
reg [ARF_N*`PRF_W-1:0] crat_q;   // committed RAT（只被 commit 更新）
reg [`PRF_N-1:0]       cfl_q;    // committed freelist（flush 時的 fl 來源）

// ---------------- RAT 讀（mux 鏈，非記憶體）----------------
function [`PRF_W-1:0] rat_rd;
    input [ARFI_W-1:0] a;
    integer k;
    begin
        rat_rd = rat_q[0 +: `PRF_W];
        for (k = 1; k < ARF_N; k = k + 1)
            if (a == k[ARFI_W-1:0]) rat_rd = rat_q[`PRF_W*k +: `PRF_W];
    end
endfunction

// ---------------- cRAT 讀（同樣是 mux 鏈）----------------
// 用完整 6-bit 架構編號比對：arch >= 32（demo 不產生）不會命中任何 entry，
// 回傳 0，後續的 (old != 0) 守衛會擋掉偽釋放。
function [`PRF_W-1:0] crat_rd;
    input [`ARF_W-1:0] a;
    integer k;
    begin
        crat_rd = {`PRF_W{1'b0}};
        for (k = 1; k < ARF_N; k = k + 1)
            if (a == {{(`ARF_W-ARFI_W){1'b0}}, k[ARFI_W-1:0]}) crat_rd = crat_q[`PRF_W*k +: `PRF_W];
    end
endfunction

// ---------------- freelist：最低可用實體暫存器 ----------------
// 回傳 {found, index}
function [`PRF_W:0] fl_pick;
    input [`PRF_N-1:0] m;
    integer k;
    begin
        fl_pick = {1'b0, {`PRF_W{1'b0}}};
        for (k = `PRF_N-1; k >= 0; k = k - 1)
            if (m[k]) fl_pick = {1'b1, k[`PRF_W-1:0]};
    end
endfunction

function [`PRF_N-1:0] fl_oh;     // one-hot 遮罩（decoder，非移位算術）
    input [`PRF_W-1:0] p;
    input              en;
    integer k;
    begin
        fl_oh = {`PRF_N{1'b0}};
        for (k = 0; k < `PRF_N; k = k + 1)
            if (en & (p == k[`PRF_W-1:0])) fl_oh[k] = 1'b1;
    end
endfunction

// ---------------- 拆 duop ----------------
wire [DW-1:0] d0 = de_duop[0*DW +: DW];
wire [DW-1:0] d1 = de_duop[1*DW +: DW];
wire [DW-1:0] d2 = de_duop[2*DW +: DW];
wire [DW-1:0] d3 = de_duop[3*DW +: DW];
wire unused_blkend = |{d0[`DUOP_BLKEND], d1[`DUOP_BLKEND],
                       d2[`DUOP_BLKEND], d3[`DUOP_BLKEND]};

wire [ARFI_W-1:0] ad0 = d0[`DUOP_D];    // 目的架構暫存器（5 bit）
wire [ARFI_W-1:0] ad1 = d1[`DUOP_D];
wire [ARFI_W-1:0] ad2 = d2[`DUOP_D];
wire [ARFI_W-1:0] ad3 = d3[`DUOP_D];

// x0 不配實體暫存器
wire dv0 = d0[`DUOP_DV] & (ad0 != 5'd0);
wire dv1 = d1[`DUOP_DV] & (ad1 != 5'd0);
wire dv2 = d2[`DUOP_DV] & (ad2 != 5'd0);
wire dv3 = d3[`DUOP_DV] & (ad3 != 5'd0);

wire nd0 = de_valid[0] & dv0;    // 本拍需要配一個實體暫存器
wire nd1 = de_valid[1] & dv1;
wire nd2 = de_valid[2] & dv2;
wire nd3 = de_valid[3] & dv3;

// bundle 內的配額序號（0..3）
wire [1:0] sl0 = 2'd0;
wire [1:0] sl1 = {1'b0, nd0};
wire [1:0] sl2 = {1'b0, nd0} + {1'b0, nd1};
wire [1:0] sl3 = {1'b0, nd0} + {1'b0, nd1} + {1'b0, nd2};

// ---------------- 4 級 freelist pick ----------------
wire [`PRF_N-1:0] m0 = fl_q & ~{{(`PRF_N-1){1'b0}}, 1'b1};   // 實體 0 保留給 x0
wire [`PRF_W:0]   r0 = fl_pick(m0);
wire [`PRF_N-1:0] m1 = m0 & ~fl_oh(r0[`PRF_W-1:0], r0[`PRF_W]);
wire [`PRF_W:0]   r1 = fl_pick(m1);
wire [`PRF_N-1:0] m2 = m1 & ~fl_oh(r1[`PRF_W-1:0], r1[`PRF_W]);
wire [`PRF_W:0]   r2 = fl_pick(m2);
wire [`PRF_N-1:0] m3 = m2 & ~fl_oh(r2[`PRF_W-1:0], r2[`PRF_W]);
wire [`PRF_W:0]   r3 = fl_pick(m3);

wire [`PRF_W-1:0] fp0 = r0[`PRF_W-1:0];
wire [`PRF_W-1:0] fp1 = r1[`PRF_W-1:0];
wire [`PRF_W-1:0] fp2 = r2[`PRF_W-1:0];
wire [`PRF_W-1:0] fp3 = r3[`PRF_W-1:0];
wire [3:0]        ffd = {r3[`PRF_W], r2[`PRF_W], r1[`PRF_W], r0[`PRF_W]};

// 依序號挑到自己的實體暫存器
wire [`PRF_W-1:0] p0 = (sl0 == 2'd0) ? fp0 : (sl0 == 2'd1) ? fp1 : (sl0 == 2'd2) ? fp2 : fp3;
wire [`PRF_W-1:0] p1 = (sl1 == 2'd0) ? fp0 : (sl1 == 2'd1) ? fp1 : (sl1 == 2'd2) ? fp2 : fp3;
wire [`PRF_W-1:0] p2 = (sl2 == 2'd0) ? fp0 : (sl2 == 2'd1) ? fp1 : (sl2 == 2'd2) ? fp2 : fp3;
wire [`PRF_W-1:0] p3 = (sl3 == 2'd0) ? fp0 : (sl3 == 2'd1) ? fp1 : (sl3 == 2'd2) ? fp2 : fp3;

wire fnd0 = (sl0 == 2'd0) ? ffd[0] : (sl0 == 2'd1) ? ffd[1] : (sl0 == 2'd2) ? ffd[2] : ffd[3];
wire fnd1 = (sl1 == 2'd0) ? ffd[0] : (sl1 == 2'd1) ? ffd[1] : (sl1 == 2'd2) ? ffd[2] : ffd[3];
wire fnd2 = (sl2 == 2'd0) ? ffd[0] : (sl2 == 2'd1) ? ffd[1] : (sl2 == 2'd2) ? ffd[2] : ffd[3];
wire fnd3 = (sl3 == 2'd0) ? ffd[0] : (sl3 == 2'd1) ? ffd[1] : (sl3 == 2'd2) ? ffd[2] : ffd[3];

// ---------------- v8 partial accept ----------------
// 可配的實體暫存器數（0..`W）。ffd 是遞減的（找得到第 k 個 -> 一定找得到第 k-1 個），
// 所以直接加總就是「這拍最多能配幾個」。
// 重點：這個值**只看 freelist 自己**，完全不看 de_valid（規則 2：nready 不得依賴 valid）。
wire [TCW-1:0] n_free = {{(TCW-1){1'b0}}, ffd[0]} + {{(TCW-1){1'b0}}, ffd[1]}
                      + {{(TCW-1){1'b0}}, ffd[2]} + {{(TCW-1){1'b0}}, ffd[3]};
wire unused_fnd = |{fnd0, fnd1, fnd2, fnd3};

// 我能收前 N 條 = min(自己配得出幾個, 下游能收幾條)
assign de_nready = (n_free < rn_nready) ? n_free : rn_nready;

// 上游送來的 bundle 是依序的 thermometer；want = 想送的條數
wire [TCW-1:0] want = de_valid[0] ? (de_valid[1] ? (de_valid[2] ?
                      (de_valid[3] ? 3'd4 : 3'd3) : 3'd2) : 3'd1) : 3'd0;

// 本模組自己的資源能吃下幾條（供 rn_valid 用，不看下游 -> 不形成 valid/ready 迴圈）
wire [TCW-1:0] n_ok    = (want < n_free) ? want : n_free;
// 實際送出 = min(want, de_nready)；與下游算出的 min(popcount(rn_valid), rn_nready) 一致
wire [TCW-1:0] n_xfer  = (n_ok  < rn_nready) ? n_ok : rn_nready;

// 接受一定是「最前面的 n 條」（規則 1）。因為是前綴，
// bundle 內旁路與配額序號完全不用改：lane j 只旁路自 lane i<j，
// 而 j 被接受 => 所有 i<j 也被接受。
wire [`W-1:0] acc;
genvar ga;
generate
for (ga = 0; ga < `W; ga = ga + 1) begin : g_acc
    localparam [TCW-1:0] GA = ga;
    assign acc[ga]      = (GA < n_xfer);
    assign rn_valid[ga] = (GA < n_ok);      // 下游只會取前 rn_nready 條
end
endgenerate

// ---------------- 來源暫存器：RAT + bundle 內旁路 ----------------
wire [`ARF_W-1:0] a1_0 = d0[`DUOP_S1];  wire [`ARF_W-1:0] a2_0 = d0[`DUOP_S2];
wire [`ARF_W-1:0] a1_1 = d1[`DUOP_S1];  wire [`ARF_W-1:0] a2_1 = d1[`DUOP_S2];
wire [`ARF_W-1:0] a1_2 = d2[`DUOP_S1];  wire [`ARF_W-1:0] a2_2 = d2[`DUOP_S2];
wire [`ARF_W-1:0] a1_3 = d3[`DUOP_S1];  wire [`ARF_W-1:0] a2_3 = d3[`DUOP_S2];

// x0 永遠 ready -> 不標 valid，IQ 就不會等它
wire s1v0 = d0[`DUOP_S1V] & (a1_0 != {`ARF_W{1'b0}});
wire s1v1 = d1[`DUOP_S1V] & (a1_1 != {`ARF_W{1'b0}});
wire s1v2 = d2[`DUOP_S1V] & (a1_2 != {`ARF_W{1'b0}});
wire s1v3 = d3[`DUOP_S1V] & (a1_3 != {`ARF_W{1'b0}});
wire s2v0 = d0[`DUOP_S2V] & (a2_0 != {`ARF_W{1'b0}});
wire s2v1 = d1[`DUOP_S2V] & (a2_1 != {`ARF_W{1'b0}});
wire s2v2 = d2[`DUOP_S2V] & (a2_2 != {`ARF_W{1'b0}});
wire s2v3 = d3[`DUOP_S2V] & (a2_3 != {`ARF_W{1'b0}});

wire hit0_1 = nd0 & (a1_1 == {{(`ARF_W-ARFI_W){1'b0}}, ad0});   // uop1 的 src1 相依於 uop0
wire hit0_2 = nd0 & (a1_2 == {{(`ARF_W-ARFI_W){1'b0}}, ad0});
wire hit1_2 = nd1 & (a1_2 == {{(`ARF_W-ARFI_W){1'b0}}, ad1});
wire hit0_3 = nd0 & (a1_3 == {{(`ARF_W-ARFI_W){1'b0}}, ad0});
wire hit1_3 = nd1 & (a1_3 == {{(`ARF_W-ARFI_W){1'b0}}, ad1});
wire hit2_3 = nd2 & (a1_3 == {{(`ARF_W-ARFI_W){1'b0}}, ad2});

wire g0_1 = nd0 & (a2_1 == {{(`ARF_W-ARFI_W){1'b0}}, ad0});     // src2 同理
wire g0_2 = nd0 & (a2_2 == {{(`ARF_W-ARFI_W){1'b0}}, ad0});
wire g1_2 = nd1 & (a2_2 == {{(`ARF_W-ARFI_W){1'b0}}, ad1});
wire g0_3 = nd0 & (a2_3 == {{(`ARF_W-ARFI_W){1'b0}}, ad0});
wire g1_3 = nd1 & (a2_3 == {{(`ARF_W-ARFI_W){1'b0}}, ad1});
wire g2_3 = nd2 & (a2_3 == {{(`ARF_W-ARFI_W){1'b0}}, ad2});

// 後面的 uop 看到前面最新的 mapping（由後往前優先）
wire [`PRF_W-1:0] ps1_0 = rat_rd(a1_0[ARFI_W-1:0]);
wire [`PRF_W-1:0] ps1_1 = hit0_1 ? p0 : rat_rd(a1_1[ARFI_W-1:0]);
wire [`PRF_W-1:0] ps1_2 = hit1_2 ? p1 : hit0_2 ? p0 : rat_rd(a1_2[ARFI_W-1:0]);
wire [`PRF_W-1:0] ps1_3 = hit2_3 ? p2 : hit1_3 ? p1 : hit0_3 ? p0 : rat_rd(a1_3[ARFI_W-1:0]);

wire [`PRF_W-1:0] ps2_0 = rat_rd(a2_0[ARFI_W-1:0]);
wire [`PRF_W-1:0] ps2_1 = g0_1 ? p0 : rat_rd(a2_1[ARFI_W-1:0]);
wire [`PRF_W-1:0] ps2_2 = g1_2 ? p1 : g0_2 ? p0 : rat_rd(a2_2[ARFI_W-1:0]);
wire [`PRF_W-1:0] ps2_3 = g2_3 ? p2 : g1_3 ? p1 : g0_3 ? p0 : rat_rd(a2_3[ARFI_W-1:0]);

// ---------------- 輸出 ruop ----------------
// 逐欄位填，不用位元連接：RUOP 的欄位寬度與整體寬度會隨 `PRF_W 改變
// （PRF sweep 時由產生器重排佈局，且可能有保留位元），走巨集逐欄位填才不會錯位。
// RUOP_ARFD/ARFDV 是 DUOP_D/DUOP_DV 的原樣複製（未經 x0 gating），供 be_rob 產生
// cmt_arf；實體暫存器仍走 RUOP_D/RUOP_DV。dst=x0 的 uop 會是 ARFDV=1 但 RUOP_DV=0，
// 此時 cmt_arf=0，commit 守衛 (cmt_arf != 0) 會擋掉，不會污染 cRAT。
// rn_valid 只由 freelist 決定，不看 rn_ready -> 不會產生 valid/ready 組合迴圈
// 每條 uop 的欄位打包成向量，generate 迴圈才能用 genvar 取（全部是常數索引）
wire [`W*`PRF_W-1:0] ps1_v = {ps1_3, ps1_2, ps1_1, ps1_0};
wire [`W*`PRF_W-1:0] ps2_v = {ps2_3, ps2_2, ps2_1, ps2_0};
wire [`W*`PRF_W-1:0] pd_v  = {p3,    p2,    p1,    p0};
wire [`W-1:0]        s1v_v = {s1v3,  s1v2,  s1v1,  s1v0};
wire [`W-1:0]        s2v_v = {s2v3,  s2v2,  s2v1,  s2v0};
wire [`W-1:0]        dv_v  = {dv3,   dv2,   dv1,   dv0};
wire [`W-1:0]        wp_v  = {d3[`DUOP_WRONGPATH], d2[`DUOP_WRONGPATH],
                              d1[`DUOP_WRONGPATH], d0[`DUOP_WRONGPATH]};
wire [`W*`UC_W-1:0] cls_v = {d3[`DUOP_CLASS], d2[`DUOP_CLASS],
                              d1[`DUOP_CLASS], d0[`DUOP_CLASS]};
wire [`W*`LAT_W-1:0] lat_v = {d3[`DUOP_LAT], d2[`DUOP_LAT],
                              d1[`DUOP_LAT], d0[`DUOP_LAT]};
wire [`W-1:0]        ms_v  = {d3[`DUOP_MEMSTORE], d2[`DUOP_MEMSTORE],
                              d1[`DUOP_MEMSTORE], d0[`DUOP_MEMSTORE]};
wire [`W-1:0]        sz_v  = {d3[`DUOP_SERIALIZE], d2[`DUOP_SERIALIZE],
                              d1[`DUOP_SERIALIZE], d0[`DUOP_SERIALIZE]};
// 架構目的暫存器：DUOP_D / DUOP_DV 的原樣複製（未經 x0 gating）
wire [`W*ARFI_W-1:0] ad_v  = {ad3, ad2, ad1, ad0};
wire [`W-1:0]        adv_v = {d3[`DUOP_DV], d2[`DUOP_DV], d1[`DUOP_DV], d0[`DUOP_DV]};

genvar gj;
generate
for (gj = 0; gj < `W; gj = gj + 1) begin : g_ruop
    reg [RW-1:0] ru;
    always @* begin
        ru = {RW{1'b0}};                       // 保留位元一律填 0
        ru[`RUOP_WRONGPATH] = wp_v[gj];
        ru[`RUOP_CLASS +: `UC_W]     = cls_v[gj*`UC_W +: `UC_W];
        ru[`RUOP_LAT +: `LAT_W]       = lat_v[gj*`LAT_W +: `LAT_W];
        ru[`RUOP_S1V]       = s1v_v[gj];
        ru[`RUOP_S1 +: `PRF_W]        = ps1_v[gj*`PRF_W +: `PRF_W];
        ru[`RUOP_S2V]       = s2v_v[gj];
        ru[`RUOP_S2 +: `PRF_W]        = ps2_v[gj*`PRF_W +: `PRF_W];
        ru[`RUOP_DV]        = dv_v[gj];
        ru[`RUOP_D +: `PRF_W]         = pd_v[gj*`PRF_W +: `PRF_W];
        ru[`RUOP_MEMSTORE]  = ms_v[gj];
        ru[`RUOP_SERIALIZE] = sz_v[gj];
        ru[`RUOP_ARFD +: `RUOP_ARFD_W]      = ad_v[gj*ARFI_W +: ARFI_W];
        ru[`RUOP_ARFDV]     = adv_v[gj];
    end
    assign rn_ruop[gj*RW +: RW] = ru;
end
endgenerate

// 只有「真的送出去」的 lane 才配實體暫存器、才寫 RAT。
// 沒送出的 lane 這拍什麼都不做（freelist 位元不清），下一拍它會從
// decode queue 的 lane 0 重新進來、重新查 RAT、重新配 —— 不會洩漏實體暫存器。
wire w0 = acc[0] & nd0;
wire w1 = acc[1] & nd1;
wire w2 = acc[2] & nd2;
wire w3 = acc[3] & nd3;

// ---------------- commit（介面 v2）----------------
// ROB 給的是「這條 uop 寫入的實體暫存器」，舊的由 cRAT 查出來釋放。
wire [`ARF_W-1:0] ca0 = cmt_arf[0*`ARF_W +: `ARF_W];
wire [`ARF_W-1:0] ca1 = cmt_arf[1*`ARF_W +: `ARF_W];
wire [`ARF_W-1:0] ca2 = cmt_arf[2*`ARF_W +: `ARF_W];
wire [`ARF_W-1:0] ca3 = cmt_arf[3*`ARF_W +: `ARF_W];
wire [`PRF_W-1:0] cp0 = cmt_prf[0*`PRF_W +: `PRF_W];
wire [`PRF_W-1:0] cp1 = cmt_prf[1*`PRF_W +: `PRF_W];
wire [`PRF_W-1:0] cp2 = cmt_prf[2*`PRF_W +: `PRF_W];
wire [`PRF_W-1:0] cp3 = cmt_prf[3*`PRF_W +: `PRF_W];

wire cv0 = cmt_valid[0] & cmt_dv[0] & (ca0 != {`ARF_W{1'b0}});
wire cv1 = cmt_valid[1] & cmt_dv[1] & (ca1 != {`ARF_W{1'b0}});
wire cv2 = cmt_valid[2] & cmt_dv[2] & (ca2 != {`ARF_W{1'b0}});
wire cv3 = cmt_valid[3] & cmt_dv[3] & (ca3 != {`ARF_W{1'b0}});

// 同一拍 bundle 內可能 commit 同一個架構暫存器（連續 WAW），
// 後面那條看到的「舊 mapping」是前面那條剛寫進去的新 mapping。
wire [`PRF_W-1:0] co0 = crat_rd(ca0);
wire [`PRF_W-1:0] co1 = (cv0 & (ca1 == ca0)) ? cp0 : crat_rd(ca1);
wire [`PRF_W-1:0] co2 = (cv1 & (ca2 == ca1)) ? cp1 :
                        (cv0 & (ca2 == ca0)) ? cp0 : crat_rd(ca2);
wire [`PRF_W-1:0] co3 = (cv2 & (ca3 == ca2)) ? cp2 :
                        (cv1 & (ca3 == ca1)) ? cp1 :
                        (cv0 & (ca3 == ca0)) ? cp0 : crat_rd(ca3);

wire fr0 = cv0 & (co0 != {`PRF_W{1'b0}});
wire fr1 = cv1 & (co1 != {`PRF_W{1'b0}});
wire fr2 = cv2 & (co2 != {`PRF_W{1'b0}});
wire fr3 = cv3 & (co3 != {`PRF_W{1'b0}});

wire [`PRF_N-1:0] alloc_mask = fl_oh(p0, w0) | fl_oh(p1, w1) |
                               fl_oh(p2, w2) | fl_oh(p3, w3);
// 舊 mapping 回收（推測與 committed 兩邊都適用）
wire [`PRF_N-1:0] free_mask  = fl_oh(co0, fr0) | fl_oh(co1, fr1) |
                               fl_oh(co2, fr2) | fl_oh(co3, fr3);
// 新 mapping 變成「架構上正在使用」-> 從 committed freelist 拿掉
wire [`PRF_N-1:0] cuse_mask  = fl_oh(cp0, cv0) | fl_oh(cp1, cv1) |
                               fl_oh(cp2, cv2) | fl_oh(cp3, cv3);

// committed freelist：先清掉被佔用的，再放回被釋放的
// （bundle 內連續 WAW 時，同一個 phys 可能同時出現在兩邊，此時「釋放」為準）
wire [`PRF_N-1:0] cfl_nxt = (cfl_q & ~cuse_mask) | free_mask;

// committed RAT 下一狀態；entry 0（x0）永遠是 phys 0
wire [ARF_N*`PRF_W-1:0] crat_nxt;
genvar gk;
generate
for (gk = 0; gk < ARF_N; gk = gk + 1) begin : g_crat
    localparam [ARFI_W-1:0] KI = gk[ARFI_W-1:0];
    assign crat_nxt[`PRF_W*gk +: `PRF_W] =
        (gk == 0)                     ? {`PRF_W{1'b0}} :
        (cv3 & (ca3 == {{(`ARF_W-ARFI_W){1'b0}}, KI}))   ? cp3 :
        (cv2 & (ca2 == {{(`ARF_W-ARFI_W){1'b0}}, KI}))   ? cp2 :
        (cv1 & (ca1 == {{(`ARF_W-ARFI_W){1'b0}}, KI}))   ? cp1 :
        (cv0 & (ca0 == {{(`ARF_W-ARFI_W){1'b0}}, KI}))   ? cp0 :
                                        crat_q[`PRF_W*gk +: `PRF_W];
end
endgenerate

// reset 狀態：arch i -> phys i（i = 0..ARF_N-1），其餘全部進 freelist。
// 不能寫成 PRF_N/2 —— PRF sweep 到 128/256 時那會平白少掉一半可配的實體暫存器。
wire [`PRF_N-1:0] fl_init = {{(`PRF_N-ARF_N){1'b1}}, {ARF_N{1'b0}}};

// ---------------- 時序：RAT / freelist ----------------
integer k;
always @(posedge clk) begin
    if (rst) begin
        for (k = 0; k < ARF_N; k = k + 1) begin
            rat_q[`PRF_W*k +: `PRF_W]  <= k[`PRF_W-1:0];
            crat_q[`PRF_W*k +: `PRF_W] <= k[`PRF_W-1:0];
        end
        fl_q  <= fl_init;
        cfl_q <= fl_init;
    end else begin
        // committed 狀態只看 commit，不受 flush 影響（被 flush 的是更年輕的 uop）
        crat_q <= crat_nxt;
        cfl_q  <= cfl_nxt;
        if (flush) begin
            // 復原到架構狀態（含本拍的 commit），而不是 identity
            rat_q <= crat_nxt;
            fl_q  <= cfl_nxt;
        end else begin
            // 每個架構暫存器一組展開的比較器；後面的 uop 優先 -> bundle 內 WAW 正確
            for (k = 1; k < ARF_N; k = k + 1) begin
                if      (w3 & (ad3 == k[ARFI_W-1:0])) rat_q[`PRF_W*k +: `PRF_W] <= p3;
                else if (w2 & (ad2 == k[ARFI_W-1:0])) rat_q[`PRF_W*k +: `PRF_W] <= p2;
                else if (w1 & (ad1 == k[ARFI_W-1:0])) rat_q[`PRF_W*k +: `PRF_W] <= p1;
                else if (w0 & (ad0 == k[ARFI_W-1:0])) rat_q[`PRF_W*k +: `PRF_W] <= p0;
            end
            fl_q <= (fl_q & ~alloc_mask) | free_mask;
        end
    end
end

// ---------------- counter（CONTRACT v8 §8 v2）----------------
// partial accept 之後「每拍恰好一個來源」不夠用（一拍可能收 2 擋 2），
// 所以每個來源都有兩個互補的量：
//   cnt_st_*   完全停擺的拍數（想送但一條都沒送出）—— 互斥、可加總
//   cnt_lost_* 損失的 uop-slot（想送 − 實際送）—— 單位是 uop，一拍可加 0..`W
// 歸因仍然是「先算自己的資源、再算下游」，兩者相加恰好等於總損失：
//   lost_rename + lost_backpressure == want - n_xfer
wire stall_all = (want != {TCW{1'b0}}) & (n_xfer == {TCW{1'b0}}) & ~flush;
wire [TCW-1:0] inc_st_rn = (stall_all & (n_free == {TCW{1'b0}})) ? 3'd1 : 3'd0;
wire [TCW-1:0] inc_st_bp = (stall_all & (n_free != {TCW{1'b0}})) ? 3'd1 : 3'd0;
wire [TCW-1:0] inc_ls_rn = flush ? 3'd0 : (want - n_ok);     // freelist 擋掉的
wire [TCW-1:0] inc_ls_bp = flush ? 3'd0 : (n_ok - n_xfer);   // 下游擋掉的

localparam NC = 4;   // 0=st_rename 1=st_backpressure 2=lost_rename 3=lost_backpressure
wire [NC*TCW-1:0] cnt_inc = {inc_ls_bp, inc_ls_rn, inc_st_bp, inc_st_rn};

reg  [NC*CW-1:0]        cnt_q;
wire [NC*9-1:0]         cnt_s0;                 // byte0 的 9-bit 和（含進位輸出）
wire [NC*(CNT_B-1)-1:0] cnt_car;                // 進到 byte 1..CNT_B-1 的進位
genvar gc, gi;
generate
for (gc = 0; gc < NC; gc = gc + 1) begin : g_cnt
    // byte0 一次加 0..`W（8-bit 加法 + 進位輸出，規則 1：沒有寬算術）
    assign cnt_s0[gc*9 +: 9] = {1'b0, cnt_q[gc*CW +: 8]}
                             + {{(9-TCW){1'b0}}, cnt_inc[gc*TCW +: TCW]};
    // byte 1..CNT_B-2 是否為 8'hFF（最高 byte 的進位沒有去處，不需要）
    // 不自我參照，避免 UNOPTFLAT 組合迴圈
    wire [CNT_B-3:0] ff;
    for (gi = 1; gi < CNT_B-1; gi = gi + 1) begin : g_ff
        assign ff[gi-1] = (cnt_q[gc*CW + 8*gi +: 8] == 8'hFF);
    end
    wire k0 = cnt_s0[gc*9 + 8];          // byte0 的進位輸出
    // 以下 5 行對應 CNT_B=6；改 CNT_B 要一起改
    assign cnt_car[gc*(CNT_B-1) + 0] = k0;
    assign cnt_car[gc*(CNT_B-1) + 1] = k0 & ff[0];
    assign cnt_car[gc*(CNT_B-1) + 2] = k0 & (&ff[1:0]);
    assign cnt_car[gc*(CNT_B-1) + 3] = k0 & (&ff[2:0]);
    assign cnt_car[gc*(CNT_B-1) + 4] = k0 & (&ff[3:0]);
end
endgenerate

integer ic, jc;
always @(posedge clk) begin
    if (rst) cnt_q <= {NC*CW{1'b0}};
    else begin
        for (jc = 0; jc < NC; jc = jc + 1) begin
            cnt_q[jc*CW +: 8] <= cnt_s0[jc*9 +: 8];
            for (ic = 1; ic < CNT_B; ic = ic + 1)
                if (cnt_car[jc*(CNT_B-1) + (ic-1)])
                    cnt_q[jc*CW + 8*ic +: 8] <= cnt_q[jc*CW + 8*ic +: 8] + 8'd1;
        end
    end
end

assign cnt_st_rename         = cnt_q[0*CW +: CW];
assign cnt_st_backpressure   = cnt_q[1*CW +: CW];
assign cnt_lost_rename       = cnt_q[2*CW +: CW];
assign cnt_lost_backpressure = cnt_q[3*CW +: CW];

// ---------------- 編譯期斷言（CONTRACT v7 §10）----------------
// 本模組的 lane 邏輯是對 `W = 4 手動展開的（d0..d3 的 bundle 內旁路鏈）。
// 若 `W 改變，這裡會在 elaboration 就因為找不到模組而報錯，
// 而不是讓寬度不符被零擴展、安靜地算出錯的結果。
generate
if (`W != 4) begin : g_assert_W
    ERROR_rn_rename_requires_W_eq_4 bad_W();
end
endgenerate

endmodule
