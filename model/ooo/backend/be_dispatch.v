`include "common/ifc.vh"
// ===================================================================
// Agent E — be_dispatch（DS 級）
//   * 從 rename 收 W 條 RUOP，all-or-nothing 收（rn_ready 是單一 wire）
//   * 以 round-robin tail 指標配置 ROB entry，依 cfg_rob_entries 折返
//   * stall 來源：rob_full / lsq_full / mshr_full / ds_ready=0 / flush（v3 起全部是真回授）
//   * counter：cnt_st_rob、cnt_st_lsq（48-bit，純累加）
//   建模規則：欄位 <= 8 bit；同步單 clock；陣列全常數索引展開；case 全有 default
//   (本模組其實沒有 case，全部 if/else)
// ===================================================================
/* verilator lint_off MULTITOP */   // 四個模組一起 lint 時本來就沒有唯一 top
module be_dispatch (
    input  wire clk, input wire rst, input wire flush,
    input  wire [`ROB_W:0]            cfg_rob_entries,
    input  wire [`W-1:0]         rn_valid,
    input  wire [`W*`RUOP_W-1:0] rn_ruop,
    output wire                  rn_ready,
    output wire [`W-1:0]         ds_valid,
    output wire [`W*`RUOP_W-1:0] ds_ruop,
    output wire [`W*`ROB_W-1:0]  ds_robidx,
    input  wire                  ds_ready,
    input  wire                  lsq_full,
    input  wire                  rob_full,     // v3: 由 be_rob 回授
    input  wire                  mshr_full,    // v3: 由 lsu_q 回授
    output wire [47:0]           cnt_st_rob,
    output wire [47:0]           cnt_st_lsq
);
    // ---------------- 狀態 ----------------
    reg  [`ROB_W-1:0]    rob_tail;
    reg  [47:0]          c_st_rob;
    reg  [47:0]          c_st_lsq;

    // ---------------- 組合暫存 ----------------
    reg  [`W*`ROB_W-1:0] idx_v;
    reg  [`ROB_W-1:0]    tail_nxt;
    reg  [`ROB_W-1:0]    cur;
    reg [`ROB_W:0] cur_p1;
    integer              di;

    // ---------------- 滿 / stall ----------------
    wire       any_in   = |rn_valid;
    wire       blocked  = rob_full | lsq_full | mshr_full | (~ds_ready);
    wire       go       = (~blocked) & (~flush);

    assign rn_ready   = go;
    assign ds_valid   = go ? rn_valid : {`W{1'b0}};
    assign ds_ruop    = rn_ruop;                            // payload 直通
    assign ds_robidx  = idx_v;
    assign cnt_st_rob = c_st_rob;
    assign cnt_st_lsq = c_st_lsq;

    // ---------------- ROB entry 配置（環形 tail） ----------------
    always @* begin
        cur   = rob_tail;
        idx_v = {(`W*`ROB_W){1'b0}};
        for (di = 0; di < `W; di = di + 1) begin
            idx_v[di*`ROB_W +: `ROB_W] = cur;
            cur_p1 = {1'b0, cur} + {{`ROB_W{1'b0}},1'b1};
            if (rn_valid[di])
                cur = (cur_p1 >= cfg_rob_entries) ? {`ROB_W{1'b0}} : cur_p1[`ROB_W-1:0];
        end
        tail_nxt = cur;
    end

    always @(posedge clk) begin
        if (rst) begin
            rob_tail <= {`ROB_W{1'b0}};
            c_st_rob <= 48'd0;
            c_st_lsq <= 48'd0;
        end else begin
            if (flush)   rob_tail <= {`ROB_W{1'b0}};
            else if (go) rob_tail <= tail_nxt;
            // stall 歸因：ROB > LSQ（IQ 滿由 be_iq 計、MSHR 滿由 lsu_q 計）
            if (any_in & rob_full & ~flush)
                c_st_rob <= c_st_rob + 48'd1;
            if (any_in & ~rob_full & lsq_full & ~flush)
                c_st_lsq <= c_st_lsq + 48'd1;
        end
    end
endmodule
