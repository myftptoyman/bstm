`include "common/ifc.vh"
// ===================================================================
// BSTM OOO top — 監督者維護，agent 不可改
// 4-issue OOO，10 級 pipeline。前端預測/cache 由離線 overlay 提供。
// ===================================================================
module ooo_top (
    input  wire                     clk,
    input  wire                     rst,

    // ---- 每 lane 配置（runtime mask） ----
    input  wire [`ROB_W:0]               cfg_rob_entries,
    input  wire [`IQ_W:0]               cfg_iq_entries,
    input  wire [`LSQ_W:0]               cfg_ldq_entries,
    input  wire [`LSQ_W:0]               cfg_stq_entries,
    input  wire [`MSHR_W:0]               cfg_mshr_entries,
    input  wire [2:0]               cfg_fetch_width,
    input  wire [2:0]               cfg_issue_width,
    input  wire [2:0]               cfg_commit_width,

    // ---- fetch buffer（runtime 填，見 CONTRACT §5） ----
    input  wire [`W-1:0]            fb_valid,
    input  wire [`W*`DUOP_W-1:0]    fb_duop,
    input  wire [7:0]               fb_fe_event,     // FE_* bits
    input  wire [`W*8-1:0]          fb_mem_event,    // MEM_* bits, per uop
    output wire [2:0]               fb_take,         // 本 cycle 消耗幾條
    output wire                     fb_redirect,     // 需要重導
    output wire                     fb_redir_shadow, // 1=跳 shadow 0=回正確路徑

    // ---- 統計 ----
    output wire [47:0]              cnt_cycles,
    output wire [47:0]              cnt_retired,
    output wire [47:0]              cnt_wrongpath,
    output wire [47:0]              cnt_st_fetch,    // 前端斷流
    output wire [47:0]              cnt_st_rename,   // v6: 只計 rename 自己的資源（freelist）
    output wire [47:0]              cnt_st_iq,       // IQ 滿
    output wire [47:0]              cnt_st_rob,      // ROB 滿
    output wire [47:0]              cnt_st_lsq,      // LDQ/STQ 滿
    output wire [47:0]              cnt_st_mshr,     // MSHR 滿
    output wire [47:0]              cnt_st_backpressure, // v6: 有能力但下游不收
    output wire [47:0]              cnt_st_refill,       // v6: flush 後 decode queue 重填
    output wire [47:0]              cnt_mispred,
    output wire [47:0]              cnt_rob_occ_sum  // 除以 cycles = 平均佔用
);
    // ================= inter-module wires =================
    wire [`W-1:0]         de_valid;   wire [`W*`DUOP_W-1:0] de_duop;
    wire                  de_ready;
    wire [`W-1:0]         rn_valid;   wire [`W*`RUOP_W-1:0] rn_ruop;
    wire                  rn_ready;
    wire [`W-1:0]         ds_valid;   wire [`W*`RUOP_W-1:0] ds_ruop;
    wire [`W*`ROB_W-1:0]  ds_robidx;
    wire                  ds_ready;

    wire [`W-1:0]         iss_valid;  wire [`W*`RUOP_W-1:0] iss_ruop;
    wire [`W*`ROB_W-1:0]  iss_robidx;

    wire [`W-1:0]         wb_valid;   wire [`W*`PRF_W-1:0]  wb_prf;
    wire [`W*`ROB_W-1:0]  wb_robidx;

    wire                  flush;      wire [`ROB_W-1:0] flush_robidx;
    wire [`W-1:0]         cmt_valid;  wire [`W*`PRF_W-1:0] cmt_prf;   // 新 mapping，非舊的
    wire [`W*`ARF_W-1:0]  cmt_arf;    wire [`W-1:0] cmt_dv;

    wire                  lsu_ready;
    wire [`W-1:0]         lsu_req_v;
    wire [`W*8-1:0]       lsu_req_ev = {(`W*8){1'b0}};  // v2: 已由 lsu_q 自建 FIFO 取代，接地
    wire [`W*`ROB_W-1:0]  lsu_req_rob;
    wire [`W-1:0]         lsu_done_v; wire [`W*`ROB_W-1:0] lsu_done_rob;
    wire [`W*`PRF_W-1:0]  lsu_done_prf;
    wire                  lsq_full, mshr_full;
    wire                  rob_full;   // v3: ROB backpressure

    wire [`PRF_N-1:0]     prf_ready;   // scoreboard，供 IQ wakeup

    // ================= Agent D =================
    fe_front u_fe (
        .clk(clk), .rst(rst), .flush(flush),
        .cfg_fetch_width(cfg_fetch_width),
        .fb_valid(fb_valid), .fb_duop(fb_duop), .fb_fe_event(fb_fe_event),
        .fb_take(fb_take), .fb_redirect(fb_redirect), .fb_redir_shadow(fb_redir_shadow),
        .mispred_in(flush),
        .de_valid(de_valid), .de_duop(de_duop), .de_ready(de_ready),
        .cnt_st_fetch(cnt_st_fetch), .cnt_st_refill(cnt_st_refill), .cnt_mispred(cnt_mispred)
    );

    rn_rename u_rn (
        .clk(clk), .rst(rst), .flush(flush),
        .de_valid(de_valid), .de_duop(de_duop), .de_ready(de_ready),
        .rn_valid(rn_valid), .rn_ruop(rn_ruop), .rn_ready(rn_ready),
        .cmt_valid(cmt_valid), .cmt_dv(cmt_dv), .cmt_arf(cmt_arf), .cmt_prf(cmt_prf),
        .cnt_st_rename(cnt_st_rename), .cnt_st_backpressure(cnt_st_backpressure)
    );

    // ================= Agent E =================
    be_dispatch u_ds (
        .clk(clk), .rst(rst), .flush(flush),
        .cfg_rob_entries(cfg_rob_entries),
        .rn_valid(rn_valid), .rn_ruop(rn_ruop), .rn_ready(rn_ready),
        .ds_valid(ds_valid), .ds_ruop(ds_ruop), .ds_robidx(ds_robidx), .ds_ready(ds_ready),
        .lsq_full(lsq_full), .rob_full(rob_full), .mshr_full(mshr_full),
        .cnt_st_rob(cnt_st_rob), .cnt_st_lsq(cnt_st_lsq)
    );

    be_iq u_iq (
        .clk(clk), .rst(rst), .flush(flush),
        .cfg_iq_entries(cfg_iq_entries), .cfg_issue_width(cfg_issue_width),
        .ds_valid(ds_valid), .ds_ruop(ds_ruop), .ds_robidx(ds_robidx), .ds_ready(ds_ready),
        .prf_ready(prf_ready),
        .iss_valid(iss_valid), .iss_ruop(iss_ruop), .iss_robidx(iss_robidx),
        .cnt_st_iq(cnt_st_iq)
    );

    be_eu u_eu (
        .clk(clk), .rst(rst), .flush(flush),
        .iss_valid(iss_valid), .iss_ruop(iss_ruop), .iss_robidx(iss_robidx),
        .lsu_req_v(lsu_req_v), .lsu_req_rob(lsu_req_rob), .lsu_ready(lsu_ready),
        .lsu_done_v(lsu_done_v), .lsu_done_rob(lsu_done_rob), .lsu_done_prf(lsu_done_prf),
        .wb_valid(wb_valid), .wb_prf(wb_prf), .wb_robidx(wb_robidx),
        .prf_ready(prf_ready)
    );

    be_rob u_rob (
        .clk(clk), .rst(rst),
        .cfg_rob_entries(cfg_rob_entries), .cfg_commit_width(cfg_commit_width),
        .ds_valid(ds_valid), .ds_ruop(ds_ruop), .ds_robidx(ds_robidx),
        .wb_valid(wb_valid), .wb_robidx(wb_robidx),
        .flush(flush), .flush_robidx(flush_robidx), .rob_full(rob_full),
        .cmt_valid(cmt_valid), .cmt_dv(cmt_dv), .cmt_arf(cmt_arf), .cmt_prf(cmt_prf),
        .cnt_cycles(cnt_cycles), .cnt_retired(cnt_retired),
        .cnt_wrongpath(cnt_wrongpath), .cnt_rob_occ_sum(cnt_rob_occ_sum)
    );

    // ================= Agent F =================
    lsu_q u_lsq (
        .clk(clk), .rst(rst), .flush(flush),
        .cfg_ldq_entries(cfg_ldq_entries), .cfg_stq_entries(cfg_stq_entries),
        .ds_valid(ds_valid), .ds_ruop(ds_ruop), .ds_robidx(ds_robidx),
        .req_v(lsu_req_v), .req_rob(lsu_req_rob), .req_ev(lsu_req_ev),
        .lsu_ready(lsu_ready), .lsq_full(lsq_full),
        .mshr_full(mshr_full),
        .done_v(lsu_done_v), .done_rob(lsu_done_rob), .done_prf(lsu_done_prf),
        .fb_mem_event(fb_mem_event),
        .fb_valid(fb_valid), .fb_duop(fb_duop), .fb_take(fb_take),
        .cnt_st_mshr(cnt_st_mshr)
    );
endmodule
