`include "common/ifc.vh"
// ===================================================================
// Agent D — fe_front : fetch + decode queue            Verilog-2005
// 埠列已凍結，不可增刪。建模硬規則見 CONTRACT.md §1：
//   1) 欄位 <= 8 bit，無寬算術（48-bit counter 用 6 段 8-bit 進位鏈）
//   2) 結構建 MAX 尺寸，cfg_fetch_width 做 runtime mask
//   3) 純同步、posedge clk + 同步 rst、無 latch、無非同步讀記憶體
//      （decode queue 是「壓縮式移位暫存器」，全部展開成 flop + mux）
//   4) 無 case，全部 if/else 或 ?: 鏈
//   5) 無 $display/$finish/initial/DPI
// ===================================================================
module fe_front (
    input  wire clk, input wire rst, input wire flush,
    input  wire [2:0]            cfg_fetch_width,
    input  wire [`W-1:0]         fb_valid,
    input  wire [`W*`DUOP_W-1:0] fb_duop,
    input  wire [7:0]            fb_fe_event,
    output wire [2:0]            fb_take,
    output wire                  fb_redirect,
    output wire                  fb_redir_shadow,
    input  wire                  mispred_in,
    output wire [`W-1:0]         de_valid,
    output wire [`W*`DUOP_W-1:0] de_duop,
    input  wire                  de_ready,
    output wire [47:0]           cnt_st_fetch,
    output wire [47:0]           cnt_mispred
);

// ---------------- 參數 ----------------
localparam DQ_N = 16;            // decode queue 深度
localparam DQ_W = `DUOP_W;       // 32

// ---------------- FE_* 事件位元（include/bstf.h）----------------
// [1:0] bubbles  [2] ubtb_hit  [3] btb_override  [4] dir_ok
// [5] tgt_ok     [6] wrongpath [7] redirect
wire [1:0] fe_bubbles  = fb_fe_event[1:0];
wire       fe_dir_ok   = fb_fe_event[4];
wire       fe_redirect = fb_fe_event[7];
wire       unused_fe_ev = |{fb_fe_event[6:5], fb_fe_event[3:2]};

// ---------------- 狀態 ----------------
reg  [DQ_N*DQ_W-1:0] dq_q;        // decode queue（head 固定在 slot 0）
reg  [4:0]           dq_cnt_q;    // 0..16
reg  [1:0]           bub_q;       // 尚未消化的前端泡泡
reg                  blk_seen_q;  // 目前 head 的 fetch block 事件已取用
reg                  shadow_q;    // 1 = 目前走 wrong path

// ================= 輸出側（送往 rename）=================
// 一次最多呈現 4 條；de_ready = 1 代表「本拍把 de_valid 全收下」（all-or-nothing）
wire [2:0] n_pres = (dq_cnt_q >= 5'd4) ? 3'd4 : dq_cnt_q[2:0];

assign de_valid = (n_pres == 3'd0) ? 4'b0000 :
                  (n_pres == 3'd1) ? 4'b0001 :
                  (n_pres == 3'd2) ? 4'b0011 :
                  (n_pres == 3'd3) ? 4'b0111 : 4'b1111;
assign de_duop  = dq_q[`W*DQ_W-1:0];

wire [2:0] n_out = de_ready ? n_pres : 3'd0;         // 本拍出隊數
wire [4:0] rem   = dq_cnt_q - {2'b00, n_out};        // 出隊後剩餘

// ================= 輸入側（從 fetch buffer 取）=================
// 前端泡泡：新 block 到達時載入 FE_BUBBLES，之後每拍遞減，期間 fb_take = 0
wire [1:0] eff_bub   = (~blk_seen_q & fb_valid[0]) ? fe_bubbles : bub_q;
wire       bub_stall = (eff_bub != 2'd0);

// 連續有效的 uop 數
wire [2:0] n_avail = fb_valid[0] ? (fb_valid[1] ? (fb_valid[2] ?
                     (fb_valid[3] ? 3'd4 : 3'd3) : 3'd2) : 3'd1) : 3'd0;

// fetch block 邊界：不跨 block 取，因為 fb_fe_event 是 per-block
wire be0 = fb_valid[0] & fb_duop[0*DQ_W + `DUOP_BLKEND];
wire be1 = fb_valid[1] & fb_duop[1*DQ_W + `DUOP_BLKEND];
wire be2 = fb_valid[2] & fb_duop[2*DQ_W + `DUOP_BLKEND];
wire be3 = fb_valid[3] & fb_duop[3*DQ_W + `DUOP_BLKEND];
wire [2:0] n_blk = be0 ? 3'd1 : be1 ? 3'd2 : be2 ? 3'd3 : be3 ? 3'd4 : 3'd5;

// queue 空間（同拍出隊可回收）
wire [4:0] free_ent = 5'd16 - rem;
wire [2:0] n_space  = (free_ent >= 5'd4) ? 3'd4 : free_ent[2:0];

// n_in = min(avail, block 邊界, cfg_fetch_width, 空間)
wire [2:0] lim_a = (n_avail < n_blk)           ? n_avail : n_blk;
wire [2:0] lim_b = (lim_a   < cfg_fetch_width) ? lim_a   : cfg_fetch_width;
wire [2:0] lim_c = (lim_b   < n_space)         ? lim_b   : n_space;
wire [2:0] n_in  = (bub_stall | flush) ? 3'd0 : lim_c;

wire blk_end_taken = (n_in != 3'd0) & (n_in >= n_blk);

// ================= redirect =================
// 取到 block 最後一條、且該 block 方向預測錯 -> 切 shadow（wrong path）
wire mis_now = blk_end_taken & fe_redirect & ~fe_dir_ok & ~shadow_q;
// 後端 flush（mispred_in）優先：回正確路徑
assign fb_redirect     = mispred_in | mis_now;
assign fb_redir_shadow = ~mispred_in;
assign fb_take         = n_in;

// ================= decode queue 下一狀態 =================
// 壓縮式移位：slot i 取 slot i+n_out，尾端補新進 uop。
// 全部是固定索引 + mux 鏈，不會推斷出記憶體。
wire [(DQ_N+4)*DQ_W-1:0] dq_pad = {{4*DQ_W{1'b0}}, dq_q};
wire [DQ_N*DQ_W-1:0]     dq_nxt;

wire [DQ_W-1:0] in0 = fb_duop[0*DQ_W +: DQ_W];
wire [DQ_W-1:0] in1 = fb_duop[1*DQ_W +: DQ_W];
wire [DQ_W-1:0] in2 = fb_duop[2*DQ_W +: DQ_W];
wire [DQ_W-1:0] in3 = fb_duop[3*DQ_W +: DQ_W];

genvar gi;
generate
for (gi = 0; gi < DQ_N; gi = gi + 1) begin : g_dq
    localparam [4:0] GI = gi[4:0];
    wire [DQ_W-1:0] c0 = dq_pad[(gi+0)*DQ_W +: DQ_W];
    wire [DQ_W-1:0] c1 = dq_pad[(gi+1)*DQ_W +: DQ_W];
    wire [DQ_W-1:0] c2 = dq_pad[(gi+2)*DQ_W +: DQ_W];
    wire [DQ_W-1:0] c3 = dq_pad[(gi+3)*DQ_W +: DQ_W];
    wire [DQ_W-1:0] c4 = dq_pad[(gi+4)*DQ_W +: DQ_W];
    wire [DQ_W-1:0] sh = (n_out == 3'd0) ? c0 :
                         (n_out == 3'd1) ? c1 :
                         (n_out == 3'd2) ? c2 :
                         (n_out == 3'd3) ? c3 : c4;
    wire [4:0] fidx  = GI - rem;                       // 填入位置（GI >= rem 時有效）
    wire       keep  = (GI < rem);
    wire       fill  = (GI >= rem) & (fidx < {2'b00, n_in});
    wire [DQ_W-1:0] fv = (fidx[1:0] == 2'd0) ? in0 :
                         (fidx[1:0] == 2'd1) ? in1 :
                         (fidx[1:0] == 2'd2) ? in2 : in3;
    assign dq_nxt[gi*DQ_W +: DQ_W] = keep ? sh : (fill ? fv : c0);
end
endgenerate

// ================= 時序 =================
always @(posedge clk) begin
    if (rst) begin
        dq_q       <= {DQ_N*DQ_W{1'b0}};
        dq_cnt_q   <= 5'd0;
        bub_q      <= 2'd0;
        blk_seen_q <= 1'b0;
        shadow_q   <= 1'b0;
    end else if (flush) begin
        // 後端 flush：清 decode queue、回正確路徑、重新抓 block 事件
        dq_cnt_q   <= 5'd0;
        bub_q      <= 2'd0;
        blk_seen_q <= 1'b0;
        shadow_q   <= 1'b0;
    end else begin
        dq_q       <= dq_nxt;
        dq_cnt_q   <= rem + {2'b00, n_in};
        bub_q      <= bub_stall ? (eff_bub - 2'd1) : 2'd0;
        blk_seen_q <= blk_end_taken ? 1'b0 :
                      ((~blk_seen_q & fb_valid[0]) ? 1'b1 : blk_seen_q);
        shadow_q   <= mis_now ? 1'b1 : shadow_q;
    end
end

// ================= counter（6 段 8-bit 進位鏈，無寬算術）=================
// cnt_st_fetch：下游可收但前端交不出 uop 的 cycle 數（真正的前端斷流）
wire c_stf_inc = de_ready & (de_valid == 4'd0) & ~flush;
reg  [47:0] c_stf_q;
wire [4:0]  c_stf_ff;   // byte 0..4 是否為 8'hFF（最高 byte 不需要）
wire [5:0]  c_stf_car;
genvar gs;
generate
for (gs = 0; gs < 5; gs = gs + 1) begin : g_stf
    assign c_stf_ff[gs] = (c_stf_q[8*gs +: 8] == 8'hFF);
end
endgenerate
assign c_stf_car[0] = c_stf_inc;
assign c_stf_car[1] = c_stf_inc & c_stf_ff[0];
assign c_stf_car[2] = c_stf_inc & (&c_stf_ff[1:0]);
assign c_stf_car[3] = c_stf_inc & (&c_stf_ff[2:0]);
assign c_stf_car[4] = c_stf_inc & (&c_stf_ff[3:0]);
assign c_stf_car[5] = c_stf_inc & (&c_stf_ff[4:0]);

// cnt_mispred：前端偵測到的方向誤預測（切 shadow 的次數）
wire c_mis_inc = mis_now;
reg  [47:0] c_mis_q;
wire [4:0]  c_mis_ff;   // byte 0..4 是否為 8'hFF（最高 byte 不需要）
wire [5:0]  c_mis_car;
genvar gm;
generate
for (gm = 0; gm < 5; gm = gm + 1) begin : g_mis
    assign c_mis_ff[gm] = (c_mis_q[8*gm +: 8] == 8'hFF);
end
endgenerate
assign c_mis_car[0] = c_mis_inc;
assign c_mis_car[1] = c_mis_inc & c_mis_ff[0];
assign c_mis_car[2] = c_mis_inc & (&c_mis_ff[1:0]);
assign c_mis_car[3] = c_mis_inc & (&c_mis_ff[2:0]);
assign c_mis_car[4] = c_mis_inc & (&c_mis_ff[3:0]);
assign c_mis_car[5] = c_mis_inc & (&c_mis_ff[4:0]);

integer ic;
always @(posedge clk) begin
    if (rst) begin
        c_stf_q <= 48'd0;
        c_mis_q <= 48'd0;
    end else begin
        for (ic = 0; ic < 6; ic = ic + 1) begin
            if (c_stf_car[ic]) c_stf_q[8*ic +: 8] <= c_stf_q[8*ic +: 8] + 8'd1;
            if (c_mis_car[ic]) c_mis_q[8*ic +: 8] <= c_mis_q[8*ic +: 8] + 8'd1;
        end
    end
end

assign cnt_st_fetch = c_stf_q;
assign cnt_mispred  = c_mis_q;

endmodule
