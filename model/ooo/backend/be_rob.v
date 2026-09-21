`include "common/ifc.vh"
// ===================================================================
// Agent E — be_rob（CM 級）+ 全域 cycle counter
//   ROB_N=64 環形佇列，有效範圍 cfg_rob_entries；dispatch 端給 robidx，
//   writeback 標記完成，head 依序 commit，一拍最多 cfg_commit_width 條。
//
//   flush：commit 走到一條 wrongpath uop 時拉 flush + flush_robidx，
//          同拍把整個 ROB 清空、head 歸零（be_dispatch 的 tail 也歸零）。
//
//   commit 介面 v3：cmt_arf 來自 RUOP_ARFD/RUOP_ARFDV（rename 填），
//                   cmt_dv = RUOP_DV（是否配了實體暫存器）；ARFDV 與 DV 獨立，
//                   dst=x0 的 uop 是 ARFDV=1 / DV=0，由 rename 端以 cmt_arf!=0 自行過濾。
//   commit 介面 v2：cmt_prf = 這條 uop **寫入**的實體暫存器（RUOP_D），
//                   舊 mapping 的釋放由 rename 的 committed-RAT 負責。
//
//   counter（48-bit 純累加）：
//     cnt_cycles      rst 解除後每拍 +1
//     cnt_retired     正確路徑 commit 數
//     cnt_wrongpath   被 flush 掉的 uop 數（flush 當拍 ROB 內剩餘的有效項）
//     cnt_rob_occ_sum 每拍累加當下佔用數（/cnt_cycles = 平均佔用）
//   cnt_mispred 由 fe_front 計，本模組不重複計。
//
//   建模規則：欄位 <= 8 bit；單 clock 同步；陣列只用常數索引展開；無 case。
// ===================================================================
/* verilator lint_off MULTITOP */   // 四個模組一起 lint 時本來就沒有唯一 top
module be_rob (
    input  wire clk, input wire rst,
    input  wire [6:0]            cfg_rob_entries,
    input  wire [2:0]            cfg_commit_width,
    input  wire [`W-1:0]         ds_valid,
    input  wire [`W*`RUOP_W-1:0] ds_ruop,
    input  wire [`W*`ROB_W-1:0]  ds_robidx,
    input  wire [`W-1:0]         wb_valid,
    input  wire [`W*`ROB_W-1:0]  wb_robidx,
    output wire                  flush,
    output wire [`ROB_W-1:0]     flush_robidx,
    output wire                  rob_full,     // v3: 剩餘空間 < W，回壓 be_dispatch
    output wire [`W-1:0]         cmt_valid,
    output wire [`W-1:0]         cmt_dv,
    output wire [`W*`ARF_W-1:0]  cmt_arf,
    output wire [`W*`PRF_W-1:0]  cmt_prf,
    output wire [47:0]           cnt_cycles,
    output wire [47:0]           cnt_retired,
    output wire [47:0]           cnt_wrongpath,
    output wire [47:0]           cnt_rob_occ_sum
);
    // ---------------------------------------------- 狀態
    reg [`ROB_N-1:0] rob_v;
    reg [`ROB_N-1:0] rob_done;
    reg [`ROB_N-1:0] rob_wp;
    reg [`ROB_N-1:0] rob_dv;
    reg [`ROB_N*5-1:0] rob_a;                 // RUOP_ARFD：架構目的暫存器 x0..x31
    reg [`ROB_N*`PRF_W-1:0] rob_d;            // RUOP_D：寫入的實體暫存器（packed，避免記憶體推斷）
    reg [`ROB_W-1:0] rob_head;
    reg [6:0]        rob_cnt;                 // 0..64
    reg [47:0]       c_cycles, c_retired, c_wrong, c_occ;

    // ---------------------------------------------- 組合暫存
    reg [`W-1:0]         c_v, c_dvm;
    reg [`W*`PRF_W-1:0]  c_prf;
    reg [`W*`ROB_W-1:0]  c_idx;
    reg [`ROB_W-1:0]     h_idx, fl_idx;
    reg [6:0]            h_p1;
    reg [2:0]            n_cmt;
    reg                  stop, do_flush;
    reg [14:0]           ent;
    reg [`W*`ARF_W-1:0]  c_arf;
    integer              cj, wi, wk;

    // 所有 ROB 欄位都是 packed vector（不用 unpacked 陣列，確保不會推斷出
    // 非同步讀的 $mem / RAMGEM），而且只用常數索引展開成 mux 鏈，
    // 不產生 $shiftx / $mul 這類需要寬運算的 cell。
    // 回傳 {v, done, wp, dv, arf[4:0], d[5:0]}
    // 註：RUOP_ARFDV 在本模組用不到 —— cmt_dv 一律看 RUOP_DV，
    //     dst=x0 的 uop 是 ARFDV=1 / DV=0，rename 端再以 cmt_arf!=0 過濾。
    function [14:0] rd_ent; input [`ROB_W-1:0] ix; integer q; begin
        rd_ent = 15'd0;
        for (q = 0; q < `ROB_N; q = q + 1)
            if (q[`ROB_W-1:0] == ix)
                rd_ent = {rob_v[q], rob_done[q], rob_wp[q], rob_dv[q],
                          rob_a[q*5 +: 5], rob_d[q*`PRF_W +: `PRF_W]};
    end endfunction

    // ---------------------------------------------- commit / flush 掃描
    always @* begin
        h_idx    = rob_head;
        n_cmt    = 3'd0;
        stop     = 1'b0;
        do_flush = 1'b0;
        fl_idx   = {`ROB_W{1'b0}};
        c_v      = {`W{1'b0}};
        c_dvm    = {`W{1'b0}};
        c_prf    = {(`W*`PRF_W){1'b0}};
        c_arf    = {(`W*`ARF_W){1'b0}};
        c_idx    = {(`W*`ROB_W){1'b0}};
        h_p1     = 7'd0;
        ent = 15'd0;
        for (cj = 0; cj < `W; cj = cj + 1) begin
            ent = rd_ent(h_idx);
            if (!stop && (cj[2:0] < cfg_commit_width) && ent[14] && ent[13]) begin
                if (ent[12]) begin
                    // 走到 wrong-path uop：不 retire，直接清管線
                    do_flush = 1'b1;
                    fl_idx   = h_idx;
                    stop     = 1'b1;
                end else begin
                    c_v[cj]   = 1'b1;
                    c_dvm[cj] = ent[11];             // 只看 RUOP_DV：有沒有配實體暫存器
                                                     // （dst=x0 時 ARFDV=1 但 DV=0，見 README）
                    c_prf[cj*`PRF_W +: `PRF_W] = ent[`PRF_W-1:0];
                    c_arf[cj*`ARF_W +: `ARF_W] = {1'b0, ent[10:6]};
                    c_idx[cj*`ROB_W +: `ROB_W] = h_idx;
                    n_cmt     = n_cmt + 3'd1;
                    h_p1      = {1'b0, h_idx} + 7'd1;
                    h_idx     = (h_p1 >= cfg_rob_entries) ? {`ROB_W{1'b0}}
                                                          : h_p1[`ROB_W-1:0];
                end
            end else begin
                stop = 1'b1;
            end
        end
    end

    wire [2:0] n_disp = {2'b0, ds_valid[0]} + {2'b0, ds_valid[1]}
                      + {2'b0, ds_valid[2]} + {2'b0, ds_valid[3]};
    wire [6:0] occ_nxt = rob_cnt + {4'b0, n_disp} - {4'b0, n_cmt};
    wire [6:0] n_kill  = rob_cnt - {4'b0, n_cmt};     // flush 當拍被丟掉的 uop 數

    assign flush           = do_flush;
    assign flush_robidx    = fl_idx;
    assign cmt_valid       = c_v;
    assign cmt_dv          = c_dvm;
    assign cmt_prf         = c_prf;
    assign cnt_cycles      = c_cycles;
    assign cnt_retired     = c_retired;
    assign cnt_wrongpath   = c_wrong;
    assign cnt_rob_occ_sum = c_occ;

    // v3：架構目的暫存器直接從 ROB entry 取出（RUOP_ARFD），不再有佔位
    assign cmt_arf = c_arf;
    // ROB 剩餘空間 < W 就回壓（用本拍開始時的佔用，不看本拍的 commit → 保守 1 拍）
    assign rob_full = (({1'b0, rob_cnt} + 8'd4) > {1'b0, cfg_rob_entries});

    // ---------------------------------------------- 時序
    always @(posedge clk) begin
        if (rst) begin
            rob_v    <= {`ROB_N{1'b0}};
            rob_done <= {`ROB_N{1'b0}};
            rob_wp   <= {`ROB_N{1'b0}};
            rob_dv   <= {`ROB_N{1'b0}};
            rob_a    <= {(`ROB_N*5){1'b0}};
            rob_head <= {`ROB_W{1'b0}};
            rob_cnt  <= 7'd0;
            c_cycles <= 48'd0;
            c_retired<= 48'd0;
            c_wrong  <= 48'd0;
            c_occ    <= 48'd0;
            rob_d    <= {(`ROB_N*`PRF_W){1'b0}};
        end else begin
            c_cycles <= c_cycles + 48'd1;
            c_occ    <= c_occ    + {41'd0, rob_cnt};
            c_retired<= c_retired+ {45'd0, n_cmt};
            if (do_flush) c_wrong <= c_wrong + {41'd0, n_kill};

            if (do_flush) begin
                rob_v    <= {`ROB_N{1'b0}};
                rob_done <= {`ROB_N{1'b0}};
                rob_head <= {`ROB_W{1'b0}};
                rob_cnt  <= 7'd0;
            end else begin
                rob_head <= h_idx;
                rob_cnt  <= occ_nxt;
                // 三種寫入依序覆蓋：commit 釋放 -> writeback 標記 -> dispatch 配置
                for (wi = 0; wi < `ROB_N; wi = wi + 1) begin
                    for (wk = 0; wk < `W; wk = wk + 1)
                        if (c_v[wk] && (c_idx[wk*`ROB_W +: `ROB_W] == wi[`ROB_W-1:0]))
                            rob_v[wi] <= 1'b0;
                    for (wk = 0; wk < `W; wk = wk + 1)
                        if (wb_valid[wk] && (wb_robidx[wk*`ROB_W +: `ROB_W] == wi[`ROB_W-1:0]))
                            rob_done[wi] <= 1'b1;
                    for (wk = 0; wk < `W; wk = wk + 1)
                        if (ds_valid[wk] && (ds_robidx[wk*`ROB_W +: `ROB_W] == wi[`ROB_W-1:0])) begin
                            rob_v[wi]    <= 1'b1;
                            rob_done[wi] <= 1'b0;
                            rob_wp[wi]   <= ds_ruop[wk*`RUOP_W + `RUOP_WRONGPATH];
                            rob_dv[wi]   <= ds_ruop[wk*`RUOP_W + `RUOP_DV];
                            rob_d[wi*`PRF_W +: `PRF_W] <= ds_ruop[wk*`RUOP_W + 2 +: `PRF_W];
                            rob_a[wi*5 +: 5]           <= ds_ruop[wk*`RUOP_W + 32 +: 5];
                        end
                end
            end
        end
    end
endmodule
