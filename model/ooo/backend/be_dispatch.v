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
    output wire [2:0]            rn_nready,    // v8: 這拍能收前幾條（0..`W）
    output wire [`W-1:0]         ds_valid,
    output wire [`W*`RUOP_W-1:0] ds_ruop,
    output wire [`W*`ROB_W-1:0]  ds_robidx,
    input  wire [2:0]            ds_nready,    // v8: IQ 能收幾條
    input  wire [2:0]            lsq_nfree,    // v8: LDQ/STQ 能收幾條
    input  wire [2:0]            rob_nfree,    // v8: ROB 能收幾條
    input  wire                  mshr_full,    // v3: 由 lsu_q 回授
    output wire [47:0]           cnt_st_rob,
    output wire [47:0]           cnt_st_lsq,
    // v8 §8：cnt_st_* 是「完全停擺的拍數」，cnt_lost_* 是「損失的 uop-slot」
    output wire [47:0]           cnt_lost_rob,
    output wire [47:0]           cnt_lost_lsq,
    output wire [47:0]           cnt_lost_iq
);
    // ---------------- 狀態 ----------------
    reg  [`ROB_W-1:0]    rob_tail;
    reg  [47:0]          c_st_rob,   c_st_lsq;
    reg  [47:0]          c_lost_rob, c_lost_lsq, c_lost_iq;

    // ---------------- 組合暫存 ----------------
    reg  [`W*`ROB_W-1:0] idx_v;
    reg  [`ROB_W-1:0]    tail_nxt;
    reg  [`ROB_W-1:0]    cur;
    reg  [`ROB_W:0]      cur_p1;
    reg  [`W-1:0]        acc;
    reg  [2:0]           pre;
    integer              di;

    // ---------------- v8 partial accept：三路取最小 ----------------
    // MSHR 滿等同 LSU 這拍收不下（子原因由 lsu_q 的 cnt_st_mshr 區分）
    wire [2:0] lsq_eff = mshr_full ? 3'd0 : lsq_nfree;
    wire [2:0] lim_a   = (rob_nfree < lsq_eff)   ? rob_nfree : lsq_eff;
    wire [2:0] lim_b   = (lim_a     < ds_nready) ? lim_a     : ds_nready;
    wire [2:0] lim     = flush ? 3'd0 : lim_b;
    // ** 完全不看 rn_valid **：符合 CONTRACT v8 規則 2，不會有 valid<->ready 組合迴圈
    assign rn_nready   = lim;

    // 依序接受最前面的 lim 條 valid（不跳號）
    wire [2:0] nv      = {2'b0, rn_valid[0]} + {2'b0, rn_valid[1]}
                       + {2'b0, rn_valid[2]} + {2'b0, rn_valid[3]};
    wire [2:0] n_sent  = (nv < lim) ? nv : lim;
    wire [2:0] n_lost  = nv - n_sent;
    wire       any_in  = |rn_valid;

    assign ds_valid   = acc;
    assign ds_ruop    = rn_ruop;                            // payload 直通
    assign ds_robidx  = idx_v;
    assign cnt_st_rob   = c_st_rob;
    assign cnt_st_lsq   = c_st_lsq;
    assign cnt_lost_rob = c_lost_rob;
    assign cnt_lost_lsq = c_lost_lsq;
    assign cnt_lost_iq  = c_lost_iq;

    // 歸因：誰是綁住的那一個（平手時 ROB > LSQ > IQ，與 v3 的順序一致）
    wire bind_rob = (rob_nfree <= lsq_eff)   & (rob_nfree <= ds_nready);
    wire bind_lsq = ~bind_rob & (lsq_eff <= ds_nready);

    // ---------------- 接受哪幾條 + ROB entry 配置（環形 tail） ----------------
    always @* begin
        cur   = rob_tail;
        idx_v = {(`W*`ROB_W){1'b0}};
        acc   = {`W{1'b0}};
        pre    = 3'd0;
        cur_p1 = {(`ROB_W+1){1'b0}};
        for (di = 0; di < `W; di = di + 1) begin
            idx_v[di*`ROB_W +: `ROB_W] = cur;
            if (rn_valid[di]) begin
                if (pre < lim) begin                 // 前 lim 條才收，依序不跳號
                    acc[di] = 1'b1;
                    cur_p1 = {1'b0, cur} + {{`ROB_W{1'b0}},1'b1};
                    cur = (cur_p1 >= cfg_rob_entries) ? {`ROB_W{1'b0}} : cur_p1[`ROB_W-1:0];
                end
                pre = pre + 3'd1;
            end
        end
        tail_nxt = cur;
    end

    always @(posedge clk) begin
        if (rst) begin
            rob_tail <= {`ROB_W{1'b0}};
            c_st_rob   <= 48'd0;
            c_st_lsq   <= 48'd0;
            c_lost_rob <= 48'd0;
            c_lost_lsq <= 48'd0;
            c_lost_iq  <= 48'd0;
        end else begin
            if (flush) rob_tail <= {`ROB_W{1'b0}};
            else       rob_tail <= tail_nxt;
            // v8 §8：cnt_st_* = 完全收不下的拍數（互斥）；cnt_lost_* = 損失的 uop-slot
            // 三個 cnt_lost_* 相加 == Σ(想送 − 實際送)，可以拿來當守恆自檢
            if (any_in & ~flush & (lim == 3'd0)) begin
                if (bind_rob)      c_st_rob <= c_st_rob + 48'd1;
                else if (bind_lsq) c_st_lsq <= c_st_lsq + 48'd1;
            end
            if (~flush & (n_lost != 3'd0)) begin
                if (bind_rob)      c_lost_rob <= c_lost_rob + {45'd0, n_lost};
                else if (bind_lsq) c_lost_lsq <= c_lost_lsq + {45'd0, n_lost};
                else               c_lost_iq  <= c_lost_iq  + {45'd0, n_lost};
            end
        end
    end
endmodule
