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
// ===================================================================
module rn_rename (
    input  wire clk, input wire rst, input wire flush,
    input  wire [`W-1:0]         de_valid,
    input  wire [`W*`DUOP_W-1:0] de_duop,
    output wire                  de_ready,
    output wire [`W-1:0]         rn_valid,
    output wire [`W*`RUOP_W-1:0] rn_ruop,
    input  wire                  rn_ready,
    input  wire [`W-1:0]         cmt_valid,
    input  wire [`W-1:0]         cmt_dv,
    input  wire [`W*`ARF_W-1:0]  cmt_arf,
    input  wire [`W*`PRF_W-1:0]  cmt_prf,      // v2: 新 mapping（非舊的）
    output wire [47:0]           cnt_st_rename
);

localparam ARF_N = 32;           // demo 只用 x0..x31
localparam DW    = `DUOP_W;
localparam RW    = `RUOP_W;

// ---------------- 狀態 ----------------
reg [ARF_N*`PRF_W-1:0] rat_q;    // 推測 RAT：架構暫存器 -> 實體暫存器
reg [`PRF_N-1:0]       fl_q;     // 推測 freelist bitmask，1 = 可配
reg [ARF_N*`PRF_W-1:0] crat_q;   // committed RAT（只被 commit 更新）
reg [`PRF_N-1:0]       cfl_q;    // committed freelist（flush 時的 fl 來源）

// ---------------- RAT 讀（mux 鏈，非記憶體）----------------
function [`PRF_W-1:0] rat_rd;
    input [4:0] a;
    integer k;
    begin
        rat_rd = rat_q[0 +: `PRF_W];
        for (k = 1; k < ARF_N; k = k + 1)
            if (a == k[4:0]) rat_rd = rat_q[`PRF_W*k +: `PRF_W];
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
            if (a == {1'b0, k[4:0]}) crat_rd = crat_q[`PRF_W*k +: `PRF_W];
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

wire [4:0] ad0 = d0[`DUOP_D];    // 目的架構暫存器（5 bit）
wire [4:0] ad1 = d1[`DUOP_D];
wire [4:0] ad2 = d2[`DUOP_D];
wire [4:0] ad3 = d3[`DUOP_D];

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

wire enough_free = (~nd0 | fnd0) & (~nd1 | fnd1) & (~nd2 | fnd2) & (~nd3 | fnd3);

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

wire hit0_1 = nd0 & (a1_1 == {1'b0, ad0});   // uop1 的 src1 相依於 uop0
wire hit0_2 = nd0 & (a1_2 == {1'b0, ad0});
wire hit1_2 = nd1 & (a1_2 == {1'b0, ad1});
wire hit0_3 = nd0 & (a1_3 == {1'b0, ad0});
wire hit1_3 = nd1 & (a1_3 == {1'b0, ad1});
wire hit2_3 = nd2 & (a1_3 == {1'b0, ad2});

wire g0_1 = nd0 & (a2_1 == {1'b0, ad0});     // src2 同理
wire g0_2 = nd0 & (a2_2 == {1'b0, ad0});
wire g1_2 = nd1 & (a2_2 == {1'b0, ad1});
wire g0_3 = nd0 & (a2_3 == {1'b0, ad0});
wire g1_3 = nd1 & (a2_3 == {1'b0, ad1});
wire g2_3 = nd2 & (a2_3 == {1'b0, ad2});

// 後面的 uop 看到前面最新的 mapping（由後往前優先）
wire [`PRF_W-1:0] ps1_0 = rat_rd(a1_0[4:0]);
wire [`PRF_W-1:0] ps1_1 = hit0_1 ? p0 : rat_rd(a1_1[4:0]);
wire [`PRF_W-1:0] ps1_2 = hit1_2 ? p1 : hit0_2 ? p0 : rat_rd(a1_2[4:0]);
wire [`PRF_W-1:0] ps1_3 = hit2_3 ? p2 : hit1_3 ? p1 : hit0_3 ? p0 : rat_rd(a1_3[4:0]);

wire [`PRF_W-1:0] ps2_0 = rat_rd(a2_0[4:0]);
wire [`PRF_W-1:0] ps2_1 = g0_1 ? p0 : rat_rd(a2_1[4:0]);
wire [`PRF_W-1:0] ps2_2 = g1_2 ? p1 : g0_2 ? p0 : rat_rd(a2_2[4:0]);
wire [`PRF_W-1:0] ps2_3 = g2_3 ? p2 : g1_3 ? p1 : g0_3 ? p0 : rat_rd(a2_3[4:0]);

// ---------------- 輸出 ruop（介面 v3：40 bit）----------------
// bit[31:0] 佈局不變；bit[37:32] 新增架構目的暫存器，供 be_rob 產生 cmt_arf。
// RUOP_ARFD/ARFDV 是 DUOP_D/DUOP_DV 的原樣複製（未經 x0 gating），
// 實體暫存器仍走 RUOP_D/RUOP_DV（dst=x0 時為 0，因為 x0 不配實體暫存器）。
// dst=x0 的 uop 會出現 ARFDV=1 但 RUOP_DV=0；此時 cmt_arf=0，
// rn_rename 的 commit 守衛 (cmt_arf != 0) 會把它丟掉，不會污染 cRAT。
// rn_valid 只由 freelist 決定，不看 rn_ready -> 不會產生 valid/ready 組合迴圈
assign rn_ruop[0*RW +: RW] = {2'b00, d0[`DUOP_DV], d0[`DUOP_D],
                              d0[`DUOP_WRONGPATH], d0[`DUOP_CLASS], d0[`DUOP_LAT],
                              s1v0, ps1_0, s2v0, ps2_0, dv0, p0,
                              d0[`DUOP_MEMSTORE], d0[`DUOP_SERIALIZE]};
assign rn_ruop[1*RW +: RW] = {2'b00, d1[`DUOP_DV], d1[`DUOP_D],
                              d1[`DUOP_WRONGPATH], d1[`DUOP_CLASS], d1[`DUOP_LAT],
                              s1v1, ps1_1, s2v1, ps2_1, dv1, p1,
                              d1[`DUOP_MEMSTORE], d1[`DUOP_SERIALIZE]};
assign rn_ruop[2*RW +: RW] = {2'b00, d2[`DUOP_DV], d2[`DUOP_D],
                              d2[`DUOP_WRONGPATH], d2[`DUOP_CLASS], d2[`DUOP_LAT],
                              s1v2, ps1_2, s2v2, ps2_2, dv2, p2,
                              d2[`DUOP_MEMSTORE], d2[`DUOP_SERIALIZE]};
assign rn_ruop[3*RW +: RW] = {2'b00, d3[`DUOP_DV], d3[`DUOP_D],
                              d3[`DUOP_WRONGPATH], d3[`DUOP_CLASS], d3[`DUOP_LAT],
                              s1v3, ps1_3, s2v3, ps2_3, dv3, p3,
                              d3[`DUOP_MEMSTORE], d3[`DUOP_SERIALIZE]};

assign rn_valid = de_valid & {`W{enough_free}};
assign de_ready = rn_ready & enough_free;

wire xfer = de_ready & (|de_valid);
wire w0 = xfer & nd0;
wire w1 = xfer & nd1;
wire w2 = xfer & nd2;
wire w3 = xfer & nd3;

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
    localparam [4:0] KI = gk[4:0];
    assign crat_nxt[`PRF_W*gk +: `PRF_W] =
        (gk == 0)                     ? {`PRF_W{1'b0}} :
        (cv3 & (ca3 == {1'b0, KI}))   ? cp3 :
        (cv2 & (ca2 == {1'b0, KI}))   ? cp2 :
        (cv1 & (ca1 == {1'b0, KI}))   ? cp1 :
        (cv0 & (ca0 == {1'b0, KI}))   ? cp0 :
                                        crat_q[`PRF_W*gk +: `PRF_W];
end
endgenerate

// reset 狀態：arch i -> phys i，freelist = phys 32..63
wire [`PRF_N-1:0] fl_init = {{(`PRF_N/2){1'b1}}, {(`PRF_N/2){1'b0}}};

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
                if      (w3 & (ad3 == k[4:0])) rat_q[`PRF_W*k +: `PRF_W] <= p3;
                else if (w2 & (ad2 == k[4:0])) rat_q[`PRF_W*k +: `PRF_W] <= p2;
                else if (w1 & (ad1 == k[4:0])) rat_q[`PRF_W*k +: `PRF_W] <= p1;
                else if (w0 & (ad0 == k[4:0])) rat_q[`PRF_W*k +: `PRF_W] <= p0;
            end
            fl_q <= (fl_q & ~alloc_mask) | free_mask;
        end
    end
end

// ---------------- counter（6 段 8-bit 進位鏈）----------------
// cnt_st_rename：有 uop 要 rename 但因 freelist 空或下游 backpressure 而停
wire c_rn_inc = (|de_valid) & (~rn_ready | ~enough_free) & ~flush;
reg  [47:0] c_rn_q;
wire [4:0]  c_rn_ff;   // byte 0..4 是否為 8'hFF（最高 byte 不需要）
wire [5:0]  c_rn_car;
genvar gr;
generate
for (gr = 0; gr < 5; gr = gr + 1) begin : g_rn
    assign c_rn_ff[gr] = (c_rn_q[8*gr +: 8] == 8'hFF);
end
endgenerate
assign c_rn_car[0] = c_rn_inc;
assign c_rn_car[1] = c_rn_inc & c_rn_ff[0];
assign c_rn_car[2] = c_rn_inc & (&c_rn_ff[1:0]);
assign c_rn_car[3] = c_rn_inc & (&c_rn_ff[2:0]);
assign c_rn_car[4] = c_rn_inc & (&c_rn_ff[3:0]);
assign c_rn_car[5] = c_rn_inc & (&c_rn_ff[4:0]);

integer ic;
always @(posedge clk) begin
    if (rst) c_rn_q <= 48'd0;
    else begin
        for (ic = 0; ic < 6; ic = ic + 1)
            if (c_rn_car[ic]) c_rn_q[8*ic +: 8] <= c_rn_q[8*ic +: 8] + 8'd1;
    end
end

assign cnt_st_rename = c_rn_q;

endmodule
