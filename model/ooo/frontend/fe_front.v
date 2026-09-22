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
    input  wire [2:0]            de_nready,    // v8: 下游這拍能收前 N 條（0..`W）
    output wire [47:0]           cnt_st_fetch,
    output wire [47:0]           cnt_st_refill,
    output wire [47:0]           cnt_lost_fetch,
    output wire [47:0]           cnt_lost_refill,
    output wire [47:0]           cnt_mispred
);

// ---------------- 參數 ----------------
localparam DQ_N  = 16;           // decode queue 深度（本模組唯一的結構尺寸）
localparam DQ_W  = `DUOP_W;      // 解碼後 uop 寬度（跟著巨集走）
// 佔用計數器要裝得下 MAX 本身（CONTRACT v7 §10 第 2 點）：
// DQ_N=16 需要 5 bit（0..16），不是 $clog2(16)=4 bit。
localparam DQ_CW = $clog2(DQ_N) + 1;
localparam [DQ_CW-1:0] DQ_NC = DQ_N;   // 宣告寬度即截斷，不用 part-select
// 一拍最多搬 `W 條，take/present 的計數寬度也從 `W 推導（fb_take 埠是 [2:0]）
localparam TCW   = $clog2(`W) + 1;
localparam [TCW-1:0]   W_T  = `W;
localparam [DQ_CW-1:0] W_C  = `W;
// CNT_B「碰巧」是 6，但它是 48-bit counter 的 8-bit 分段數，
// 與 `PRF_W / `ROB_W 無關，做 PRF/ROB sweep 時不可跟著改。
localparam CNT_B = 6;
localparam CW    = CNT_B * 8;    // = 48，counter 埠寬

// ---------------- FE_* 事件位元（include/bstf.h）----------------
// [1:0] bubbles  [2] ubtb_hit  [3] btb_override  [4] dir_ok
// [5] tgt_ok     [6] wrongpath [7] redirect
wire [1:0] fe_bubbles  = fb_fe_event[1:0];
wire       fe_dir_ok   = fb_fe_event[4];
wire       fe_redirect = fb_fe_event[7];
wire       unused_fe_ev = |{fb_fe_event[6:5], fb_fe_event[3:2]};

// ---------------- 狀態 ----------------
reg  [DQ_N*DQ_W-1:0] dq_q;        // decode queue（head 固定在 slot 0）
reg  [DQ_CW-1:0]     dq_cnt_q;    // 0..DQ_N（含 DQ_N 本身）
reg  [1:0]           bub_q;       // 尚未消化的前端泡泡
reg                  blk_seen_q;  // 目前 head 的 fetch block 事件已取用
reg                  shadow_q;    // 1 = 目前走 wrong path
reg                  refill_q;    // 1 = flush 之後還沒成功送出過 uop（重填視窗）

// ================= 輸出側（送往 rename）=================
// 一次最多呈現 `W 條；下游用 de_nready 告訴我們這拍收得下前幾條（v8 partial accept）
wire [TCW-1:0] n_pres = (dq_cnt_q >= W_C) ? W_T : dq_cnt_q[TCW-1:0];

// thermometer：slot i 有效 <=> i < n_pres（寬度跟著 `W 走，不寫死 4'b....）
genvar gv;
generate
for (gv = 0; gv < `W; gv = gv + 1) begin : g_dev
    localparam [TCW-1:0] GV = gv;
    assign de_valid[gv] = (GV < n_pres);
end
endgenerate
assign de_duop  = dq_q[`W*DQ_W-1:0];

// v8 partial accept：下游收前 de_nready 條，剩下的留在 queue 裡依序重送（規則 3）。
// 壓縮式移位佇列天生支援任意 n_out，不用額外的保留邏輯。
wire [TCW-1:0] n_out = (n_pres < de_nready) ? n_pres : de_nready;         // 本拍出隊數
wire [DQ_CW-1:0] rem = dq_cnt_q - {{(DQ_CW-TCW){1'b0}}, n_out};   // 出隊後剩餘

// ================= 輸入側（從 fetch buffer 取）=================
// 前端泡泡：新 block 到達時載入 FE_BUBBLES，之後每拍遞減，期間 fb_take = 0
wire [1:0] eff_bub   = (~blk_seen_q & fb_valid[0]) ? fe_bubbles : bub_q;
wire       bub_stall = (eff_bub != 2'd0);

// 連續有效的 uop 數
wire [TCW-1:0] n_avail = fb_valid[0] ? (fb_valid[1] ? (fb_valid[2] ?
                     (fb_valid[3] ? 3'd4 : 3'd3) : 3'd2) : 3'd1) : 3'd0;

// fetch block 邊界：不跨 block 取，因為 fb_fe_event 是 per-block
wire be0 = fb_valid[0] & fb_duop[0*DQ_W + `DUOP_BLKEND];
wire be1 = fb_valid[1] & fb_duop[1*DQ_W + `DUOP_BLKEND];
wire be2 = fb_valid[2] & fb_duop[2*DQ_W + `DUOP_BLKEND];
wire be3 = fb_valid[3] & fb_duop[3*DQ_W + `DUOP_BLKEND];
wire [TCW-1:0] n_blk = be0 ? 3'd1 : be1 ? 3'd2 : be2 ? 3'd3 : be3 ? 3'd4 : 3'd5;

// queue 空間（同拍出隊可回收）
wire [DQ_CW-1:0] free_ent = DQ_NC - rem;
wire [TCW-1:0]   n_space  = (free_ent >= W_C) ? W_T : free_ent[TCW-1:0];

// n_in = min(avail, block 邊界, cfg_fetch_width, 空間)
wire [TCW-1:0] lim_a = (n_avail < n_blk)           ? n_avail : n_blk;
wire [TCW-1:0] lim_b = (lim_a   < cfg_fetch_width) ? lim_a   : cfg_fetch_width;
wire [TCW-1:0] lim_c = (lim_b   < n_space)         ? lim_b   : n_space;
wire [TCW-1:0] n_in  = (bub_stall | flush) ? 3'd0 : lim_c;

wire blk_end_taken = (n_in != {TCW{1'b0}}) & (n_in >= n_blk);

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
        dq_cnt_q   <= {DQ_CW{1'b0}};
        bub_q      <= 2'd0;
        blk_seen_q <= 1'b0;
        shadow_q   <= 1'b0;
        refill_q   <= 1'b0;      // reset 不是 flush：冷啟動的填管線算 cnt_st_fetch
    end else if (flush) begin
        // 後端 flush：清 decode queue、回正確路徑、重新抓 block 事件
        dq_cnt_q   <= {DQ_CW{1'b0}};
        bub_q      <= 2'd0;
        blk_seen_q <= 1'b0;
        shadow_q   <= 1'b0;
        refill_q   <= 1'b1;      // 進入重填視窗
    end else begin
        dq_q       <= dq_nxt;
        dq_cnt_q   <= rem + {{(DQ_CW-TCW){1'b0}}, n_in};
        bub_q      <= bub_stall ? (eff_bub - 2'd1) : 2'd0;
        blk_seen_q <= blk_end_taken ? 1'b0 :
                      ((~blk_seen_q & fb_valid[0]) ? 1'b1 : blk_seen_q);
        shadow_q   <= mis_now ? 1'b1 : shadow_q;
        // 只要這拍真的送出至少一條 uop 就離開重填視窗
        refill_q   <= (n_out != {TCW{1'b0}}) ? 1'b0 : refill_q;
    end
end

// ================= counter（6 段 8-bit 進位鏈，無寬算術）=================
// ---------------- counter（CONTRACT v8 §8 v2）----------------
// 歸因原則：一個 stall cycle 只能有一個擁有者，而且必須是真正的來源。
// partial accept 之後每個來源都有兩個互補的量：
//   cnt_st_*   完全停擺的拍數（下游收得下、但前端一條都交不出來）
//   cnt_lost_* 損失的 uop-slot（下游開出的名額 − 前端真的填上的），一拍 0..`W
// 「下游收得下」現在是 de_nready != 0（不是舊的 de_ready）。
// flush 當拍兩邊都不計（那是誤預測懲罰本身，事件記在 cnt_mispred）。
// refill_q 把「flush 後的重填」從「真前端斷流」裡分出來，兩者互斥、相加守恆。
wire starve_all = (de_nready != 3'd0) & (n_pres == {TCW{1'b0}}) & ~flush;
wire [TCW-1:0] lost_slot = flush ? {TCW{1'b0}} : (de_nready - n_out);

wire [TCW-1:0] inc_st_fe = (starve_all & ~refill_q) ? 3'd1 : 3'd0;
wire [TCW-1:0] inc_st_rf = (starve_all &  refill_q) ? 3'd1 : 3'd0;
wire [TCW-1:0] inc_ls_fe = refill_q ? {TCW{1'b0}} : lost_slot;
wire [TCW-1:0] inc_ls_rf = refill_q ? lost_slot   : {TCW{1'b0}};

// cnt_mispred 仍是「事件數」（切 shadow 的次數），不是拍數
wire [TCW-1:0] inc_mis   = mis_now ? 3'd1 : 3'd0;

localparam NC = 5;   // 0=st_fetch 1=st_refill 2=lost_fetch 3=lost_refill 4=mispred
wire [NC*TCW-1:0] cnt_inc = {inc_mis, inc_ls_rf, inc_ls_fe, inc_st_rf, inc_st_fe};

reg  [NC*CW-1:0]        cnt_q;
wire [NC*9-1:0]         cnt_s0;
wire [NC*(CNT_B-1)-1:0] cnt_car;
genvar gc, gb;
generate
for (gc = 0; gc < NC; gc = gc + 1) begin : g_cnt
    // byte0 一次加 0..`W（8-bit 加法 + 進位輸出，規則 1：沒有寬算術）
    assign cnt_s0[gc*9 +: 9] = {1'b0, cnt_q[gc*CW +: 8]}
                             + {{(9-TCW){1'b0}}, cnt_inc[gc*TCW +: TCW]};
    // byte 1..CNT_B-2 是否為 8'hFF；不自我參照，避免 UNOPTFLAT 組合迴圈
    wire [CNT_B-3:0] ff;
    for (gb = 1; gb < CNT_B-1; gb = gb + 1) begin : g_ff
        assign ff[gb-1] = (cnt_q[gc*CW + 8*gb +: 8] == 8'hFF);
    end
    wire k0 = cnt_s0[gc*9 + 8];
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

assign cnt_st_fetch    = cnt_q[0*CW +: CW];
assign cnt_st_refill   = cnt_q[1*CW +: CW];
assign cnt_lost_fetch  = cnt_q[2*CW +: CW];
assign cnt_lost_refill = cnt_q[3*CW +: CW];
assign cnt_mispred     = cnt_q[4*CW +: CW];

// ---------------- 編譯期斷言（CONTRACT v7 §10）----------------
// 本模組的 lane 邏輯是對 `W = 4 手動展開的（n_avail / n_blk / 填入 mux）。
// 若 `W 改變，這裡會在 elaboration 就因為找不到模組而報錯，
// 而不是讓寬度不符被零擴展、安靜地算出錯的結果。
generate
if (`W != 4) begin : g_assert_W
    ERROR_fe_front_requires_W_eq_4 bad_W();
end
endgenerate

endmodule
