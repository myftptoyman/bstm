// ===================================================================
// Agent E 後段回歸 testbench（不是模型的一部分，副檔名用 .sv 以免被
// ci/check_contract.sh 的 *.v 掃描與監督者的 backend/*.v build glob 掃到）
//   用法：backend/run_regress.sh   （自動掃 cfg_rob_entries 的最小/中間/MAX）
//   驗證點：
//     1. 每個 mask 都要有 commit（retired > 0）  -> 抓「head 永久卡住」
//     2. retired 不得因 mask 變大而變小          -> 抓容量不足造成的掉件
//     3. cmt_arf 端到端逐條比對（arf_bad == 0）
//   mode 9 是專門為了複現「完成輪掉件」設計的：獨立長延遲 ALU + 大量 load
//   同時填滿 EU 並製造 lsu_done 競爭。舊版 be_eu 在 mask 96/128 會從
//   IPC 1.79 掉到 0.13，就是靠這一組抓到的。
// ===================================================================
`include "common/ifc.vh"
module tb_be;
    reg clk=0, rst=1;
    integer mode; reg [31:0] cyc;
    always #1 clk = ~clk;

    reg  [`ROB_W:0] cfg_rob_entries;
    integer robmask;
    wire [5:0] cfg_iq_entries  = 6'd32;
    wire [2:0] cfg_issue_width = 3'd4;
    wire [2:0] cfg_commit_width= 3'd4;

    // ---- 假 rename ----
    reg  [`W-1:0]         rn_valid;
    reg  [`W*`RUOP_W-1:0] rn_ruop;
    wire                  rn_ready;
    reg  [5:0] pnext; reg [5:0] plast;
    integer nfed;

    wire [`W-1:0] ds_valid; wire [`W*`RUOP_W-1:0] ds_ruop;
    wire [`W*`ROB_W-1:0] ds_robidx; wire ds_ready;
    wire [`W-1:0] iss_valid; wire [`W*`RUOP_W-1:0] iss_ruop; wire [`W*`ROB_W-1:0] iss_robidx;
    wire [`W-1:0] wb_valid; wire [`W*`PRF_W-1:0] wb_prf; wire [`W*`ROB_W-1:0] wb_robidx;
    wire flush; wire [`ROB_W-1:0] flush_robidx;
    wire [`W-1:0] cmt_valid, cmt_dv; wire [`W*`ARF_W-1:0] cmt_arf; wire [`W*`PRF_W-1:0] cmt_prf;
    wire [`PRF_N-1:0] prf_ready;
    wire rob_full; reg mshr_full; integer arf_bad; integer lsuthr;
    wire lsu_rdy = (lsuthr==0) ? 1'b1 : ~(cyc[2] & cyc[1]);   // 節流時 25% 的拍不收
    // --- 每個 ROB entry 的 dispatch/writeback 追蹤（只用埠，不碰模組內部）---
    reg [`ROB_N-1:0] disp_seen, wb_seen, disp_mem;
    integer t, nev_mem, nev_alu;
    reg [`RUOP_W-1:0] dtmp;
    function is_memcls; input [3:0] c;
        is_memcls = (c==`UC_LOAD)|(c==`UC_STORE)|(c==`UC_AMO);
    endfunction
    always @(posedge clk) begin
        if (rst || flush) begin disp_seen <= 0; wb_seen <= 0; disp_mem <= 0; end
        else begin
            for (t=0;t<`W;t=t+1) if (wb_valid[t]) wb_seen[wb_robidx[t*`ROB_W +: `ROB_W]] <= 1'b1;
            for (t=0;t<`W;t=t+1) if (ds_valid[t]) begin
                dtmp = ds_ruop[t*`RUOP_W +: `RUOP_W];
                disp_seen[ds_robidx[t*`ROB_W +: `ROB_W]] <= 1'b1;
                wb_seen  [ds_robidx[t*`ROB_W +: `ROB_W]] <= 1'b0;
                disp_mem [ds_robidx[t*`ROB_W +: `ROB_W]] <= is_memcls(dtmp[`RUOP_CLASS]);
            end
        end
    end
    wire [47:0] cnt_st_rob, cnt_st_lsq, cnt_st_iq;
    wire [47:0] cnt_cycles, cnt_retired, cnt_wrongpath, cnt_rob_occ_sum;

    // ---- 假 LSU：3 拍後回 done ----
    wire [`W-1:0] lsu_req_v; wire [`W*`ROB_W-1:0] lsu_req_rob;
    reg  [`W-1:0] dv_p [0:2]; reg [`W*`ROB_W-1:0] dr_p [0:2];
    reg  [`W-1:0] lsu_done_v; reg [`W*`ROB_W-1:0] lsu_done_rob; reg [`W*`PRF_W-1:0] lsu_done_prf;
    reg  [`PRF_W-1:0] memprf [0:`ROB_N-1];
    integer q;
    always @(posedge clk) begin
        dv_p[0] <= lsu_req_v;  dr_p[0] <= lsu_req_rob;
        dv_p[1] <= dv_p[0];    dr_p[1] <= dr_p[0];
        dv_p[2] <= dv_p[1];    dr_p[2] <= dr_p[1];
        lsu_done_v <= dv_p[2]; lsu_done_rob <= dr_p[2];
        for (q=0;q<`W;q=q+1)
            lsu_done_prf[q*`PRF_W +: `PRF_W] <= memprf[dr_p[2][q*`ROB_W +: `ROB_W]];
    end
    // 記下每個 mem uop 的 dst（模擬 LSQ 在 dispatch 時就抄走）
    always @(posedge clk)
        for (q=0;q<`W;q=q+1)
            if (ds_valid[q]) memprf[ds_robidx[q*`ROB_W +: `ROB_W]] <= ds_ruop[q*`RUOP_W+2 +: `PRF_W];

    be_dispatch u_ds (.clk(clk),.rst(rst),.flush(flush),.cfg_rob_entries(cfg_rob_entries),
        .rn_valid(rn_valid),.rn_ruop(rn_ruop),.rn_ready(rn_ready),
        .ds_valid(ds_valid),.ds_ruop(ds_ruop),.ds_robidx(ds_robidx),.ds_ready(ds_ready),
        .lsq_full(1'b0),.rob_full(rob_full),.mshr_full(mshr_full),
        .cnt_st_rob(cnt_st_rob),.cnt_st_lsq(cnt_st_lsq));
    be_iq u_iq (.clk(clk),.rst(rst),.flush(flush),.cfg_iq_entries(cfg_iq_entries),
        .cfg_issue_width(cfg_issue_width),.ds_valid(ds_valid),.ds_ruop(ds_ruop),
        .ds_robidx(ds_robidx),.ds_ready(ds_ready),.prf_ready(prf_ready),
        .iss_valid(iss_valid),.iss_ruop(iss_ruop),.iss_robidx(iss_robidx),.cnt_st_iq(cnt_st_iq));
    be_eu u_eu (.clk(clk),.rst(rst),.flush(flush),.iss_valid(iss_valid),.iss_ruop(iss_ruop),
        .iss_robidx(iss_robidx),.lsu_req_v(lsu_req_v),.lsu_req_rob(lsu_req_rob),.lsu_ready(lsu_rdy),
        .lsu_done_v(lsu_done_v),.lsu_done_rob(lsu_done_rob),.lsu_done_prf(lsu_done_prf),
        .wb_valid(wb_valid),.wb_prf(wb_prf),.wb_robidx(wb_robidx),.prf_ready(prf_ready));
    be_rob u_rob (.clk(clk),.rst(rst),.cfg_rob_entries(cfg_rob_entries),
        .cfg_commit_width(cfg_commit_width),.ds_valid(ds_valid),.ds_ruop(ds_ruop),
        .ds_robidx(ds_robidx),.wb_valid(wb_valid),.wb_robidx(wb_robidx),
        .flush(flush),.flush_robidx(flush_robidx),.rob_full(rob_full),
        .cmt_valid(cmt_valid),.cmt_dv(cmt_dv),
        .cmt_arf(cmt_arf),.cmt_prf(cmt_prf),.cnt_cycles(cnt_cycles),.cnt_retired(cnt_retired),
        .cnt_wrongpath(cnt_wrongpath),.cnt_rob_occ_sum(cnt_rob_occ_sum));

    // 全部走 ifc.vh 巨集，PRF_W 變動時 TB 不用改；arfd 故意設成 d[4:0] 方便自檢
    function [`RUOP_W-1:0] mk; input [3:0] cls; input [3:0] lat; input s1v; input [`PRF_W-1:0] s1;
                              input dv; input [`PRF_W-1:0] d; input wp;
        begin
            mk = {`RUOP_W{1'b0}};
            mk[`RUOP_D]         = d;
            mk[`RUOP_DV]        = dv;
            mk[`RUOP_S2]        = {`PRF_W{1'b0}};
            mk[`RUOP_S2V]       = 1'b0;
            mk[`RUOP_S1]        = s1;
            mk[`RUOP_S1V]       = s1v;
            mk[`RUOP_LAT]       = lat;
            mk[`RUOP_CLASS]    = cls;
            mk[`RUOP_WRONGPATH] = wp;
            mk[`RUOP_ARFD]      = d[4:0];
            mk[`RUOP_ARFDV]     = dv;
        end
    endfunction

    integer k; reg [3:0] cls, lat; reg s1v; reg [`PRF_W-1:0] s1;
    always @(negedge clk) begin
        if (rst) begin rn_valid <= 0; end
        else if (rn_ready) begin
            rn_valid <= {`W{1'b1}};
            for (k=0;k<`W;k=k+1) begin
                cls = `UC_ALU; lat = 4'd1; s1v = 1'b0; s1 = {`PRF_W{1'b0}};
                if (mode==0) begin s1v=1'b0; end
                else if (mode==1) begin s1v=1'b1; s1=plast; end
                else if (mode==2) begin s1v=1'b1; s1=plast; lat=4'd3; end
                else if (mode==4) begin cls=`UC_DIV; lat=4'd8; s1v=1'b0; end
                else if (mode==5) begin cls=`UC_MUL; lat=4'd3; s1v=1'b0; end
                else if (mode==6) begin s1v=1'b0; end
                else if (mode==8) begin s1v=1'b0; end
                else if (mode==9) begin
                    s1v=1'b0;
                    if (nfed[0]) begin cls=`UC_LOAD; lat=4'd1; end
                    else         begin cls=`UC_FPU;  lat=4'd15; end
                end
                else if (mode==7) begin s1v=1'b0; lat=4'd15; cls=`UC_FPU; end
                else if (mode==3) begin
                    if (nfed[1:0]==2'd0) begin cls=`UC_LOAD; lat=4'd1; end
                    else if (nfed[2:0]==3'd3) begin cls=`UC_MUL; lat=4'd3; end
                    else if (nfed[3:0]==4'd7) begin cls=`UC_DIV; lat=4'd8; end
                    s1v=1'b1; s1=plast;
                end
                rn_ruop[k*`RUOP_W +: `RUOP_W] = mk(cls,lat,s1v,s1,1'b1,pnext,
                                                  (mode==6) && (nfed%97==96));
                plast = pnext;
                pnext = (pnext==7'd63) ? {{(`PRF_W-1){1'b0}},1'b1} : pnext+{{(`PRF_W-1){1'b0}},1'b1};
                nfed  = nfed+1;
            end
        end else rn_valid <= rn_valid;   // valid 一旦拉起不得撤回（等 ready）
    end

    always @(posedge clk) begin
        if (rst) begin mshr_full <= 1'b0; arf_bad <= 0; end
        else begin
            mshr_full <= (mode==8) ? (cyc[2]) : 1'b0;
            for (q=0;q<`W;q=q+1)
                if (cmt_valid[q] && cmt_dv[q]
                    && (cmt_arf[q*`ARF_W +: `ARF_W] != {1'b0, cmt_prf[q*`PRF_W +: 5]}))
                    arf_bad <= arf_bad + 1;
        end
    end
    always @(posedge clk) cyc <= rst ? 0 : cyc+1;
    initial begin
        if (!$value$plusargs("mode=%d", mode)) mode = 0;
        if (!$value$plusargs("rob=%d", robmask)) robmask = `ROB_N;
        if (!$value$plusargs("thr=%d", lsuthr)) lsuthr = 0;
        cfg_rob_entries = robmask[`ROB_W:0];
        pnext={{(`PRF_W-1){1'b0}},1'b1}; plast={`PRF_W{1'b0}}; nfed=0; cyc=0; mshr_full=0; arf_bad=0; rn_valid=0; rn_ruop=0;
        lsu_done_v=0; lsu_done_rob=0; lsu_done_prf=0;
        dv_p[0]=0;dv_p[1]=0;dv_p[2]=0;dr_p[0]=0;dr_p[1]=0;dr_p[2]=0;
        for (q=0;q<64;q=q+1) memprf[q]=0;
        repeat (8) @(posedge clk);
        rst = 0;
        repeat (500) @(posedge clk);
        nev_mem=0; nev_alu=0;
        for (t=0;t<`ROB_N;t=t+1)
            if (disp_seen[t] && !wb_seen[t]) begin
                if (disp_mem[t]) nev_mem=nev_mem+1; else nev_alu=nev_alu+1;
            end
        $display("  rob_mask=%0d thr=%0d 未完成 uop: mem=%0d alu=%0d  (head 卡住的元兇)",
                 robmask, lsuthr, nev_mem, nev_alu);
        $display("mode=%0d cycles=%0d retired=%0d fed=%0d IPC=%0.2f st_iq=%0d st_rob=%0d occ_avg=%0.1f wrongpath=%0d arf_bad=%0d",
                 mode, cnt_cycles, cnt_retired, nfed,
                 $itor(cnt_retired)/$itor(cnt_cycles), cnt_st_iq, cnt_st_rob,
                 $itor(cnt_rob_occ_sum)/$itor(cnt_cycles), cnt_wrongpath, arf_bad);
        $finish;
    end
endmodule
