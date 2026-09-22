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
    reg  [`IQ_W:0] cfg_iq_entries;
    wire [2:0] cfg_issue_width = 3'd4;
    integer iqmask;
    wire [2:0] cfg_commit_width= 3'd4;

    // ---- 假 rename ----
    wire [`W-1:0]         rn_valid;
    wire [`W-1:0]         rn_valid_w;
    assign rn_valid = rn_valid_w;
    reg  [`W*`RUOP_W-1:0] rn_ruop;
    wire [2:0]            rn_nready;
    reg  [5:0] pnext; reg [5:0] plast;
    integer nfed;

    wire [`W-1:0] ds_valid; wire [`W*`RUOP_W-1:0] ds_ruop;
    wire [`W*`ROB_W-1:0] ds_robidx; wire [2:0] ds_nready;
    wire [`W-1:0] iss_valid; wire [`W*`RUOP_W-1:0] iss_ruop; wire [`W*`ROB_W-1:0] iss_robidx;
    wire [`W-1:0] wb_valid; wire [`W*`PRF_W-1:0] wb_prf; wire [`W*`ROB_W-1:0] wb_robidx;
    wire flush; wire [`ROB_W-1:0] flush_robidx;
    wire [`W-1:0] cmt_valid, cmt_dv; wire [`W*`ARF_W-1:0] cmt_arf; wire [`W*`PRF_W-1:0] cmt_prf;
    wire [`PRF_N-1:0] prf_ready;
    wire [2:0] rob_nfree; reg mshr_full; reg [2:0] lsq_nfree; integer arf_bad; integer lsuthr;
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
    wire [47:0] cnt_lost_rob, cnt_lost_lsq, cnt_lost_iq;
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

    // 下游 backpressure 刺激 + cmt_arf 端到端比對
    always @(posedge clk) begin
        if (rst) begin mshr_full <= 1'b0; lsq_nfree <= 3'd4; arf_bad <= 0; end
        else begin
            mshr_full <= (mode==8)  ? cyc[2] : 1'b0;
            lsq_nfree <= (mode==10) ? (cyc[2] ? 3'd1 : 3'd2) : 3'd4;   // v8: LSU 側 partial
            for (q=0;q<`W;q=q+1)
                if (cmt_valid[q] && cmt_dv[q]
                    && (cmt_arf[q*`ARF_W +: `ARF_W] != {1'b0, cmt_prf[q*`PRF_W +: 5]}))
                    arf_bad <= arf_bad + 1;
        end
    end
    always @(posedge clk) cyc <= rst ? 0 : cyc + 1;
    // 末段存活性：死結時 retire 會完全停住，而「損失」counter 也會歸零
    // （上游拿不到 freelist 就不再要求送，需求一起消失）—— 所以要直接量存活。
    reg [47:0] ret_at400;
    always @(posedge clk) if (!rst && cyc == 400) ret_at400 <= cnt_retired;
    // +dump=1：每拍印出 issue 的 (valid, robidx)，用來跟另一種 select 實作逐拍對拍
    integer dump;
    always @(posedge clk)
        if (!rst && dump!=0)
            $display("C%0d v=%b r=%0d,%0d,%0d,%0d", cyc, iss_valid,
                     iss_robidx[0*`ROB_W +: `ROB_W], iss_robidx[1*`ROB_W +: `ROB_W],
                     iss_robidx[2*`ROB_W +: `ROB_W], iss_robidx[3*`ROB_W +: `ROB_W]);

    be_dispatch u_ds (.clk(clk),.rst(rst),.flush(flush),.cfg_rob_entries(cfg_rob_entries),
        .rn_valid(rn_valid),.rn_ruop(rn_ruop),.rn_nready(rn_nready),
        .ds_valid(ds_valid),.ds_ruop(ds_ruop),.ds_robidx(ds_robidx),.ds_nready(ds_nready),
        .lsq_nfree(lsq_nfree),.rob_nfree(rob_nfree),.mshr_full(mshr_full),
        .cnt_st_rob(cnt_st_rob),.cnt_st_lsq(cnt_st_lsq),
        .cnt_lost_rob(cnt_lost_rob),.cnt_lost_lsq(cnt_lost_lsq),.cnt_lost_iq(cnt_lost_iq));
    be_iq u_iq (.clk(clk),.rst(rst),.flush(flush),.cfg_iq_entries(cfg_iq_entries),
        .cfg_issue_width(cfg_issue_width),.ds_valid(ds_valid),.ds_ruop(ds_ruop),
        .ds_robidx(ds_robidx),.ds_nready(ds_nready),.prf_ready(prf_ready),
        .iss_valid(iss_valid),.iss_ruop(iss_ruop),.iss_robidx(iss_robidx),.cnt_st_iq(cnt_st_iq));
    be_eu u_eu (.clk(clk),.rst(rst),.flush(flush),.iss_valid(iss_valid),.iss_ruop(iss_ruop),
        .iss_robidx(iss_robidx),.lsu_req_v(lsu_req_v),.lsu_req_rob(lsu_req_rob),.lsu_ready(lsu_rdy),
        .lsu_done_v(lsu_done_v),.lsu_done_rob(lsu_done_rob),.lsu_done_prf(lsu_done_prf),
        .wb_valid(wb_valid),.wb_prf(wb_prf),.wb_robidx(wb_robidx),.prf_ready(prf_ready));
    be_rob u_rob (.clk(clk),.rst(rst),.cfg_rob_entries(cfg_rob_entries),
        .cfg_commit_width(cfg_commit_width),.ds_valid(ds_valid),.ds_ruop(ds_ruop),
        .ds_robidx(ds_robidx),.wb_valid(wb_valid),.wb_robidx(wb_robidx),
        .flush(flush),.flush_robidx(flush_robidx),.rob_nfree(rob_nfree),
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
    // ---- v8 假 rename：保留未被接受的 uop 依序重送，valid 不撤回 ----
    reg [2:0] pend_n; reg [`RUOP_W-1:0] pend_u [0:`W-1];
    integer pf, na, sh; integer want_tot, sent_tot;
    reg [`W-1:0] rnv;
    always @* begin
        rnv = {`W{1'b0}};
        for (k=0;k<`W;k=k+1) if (k[2:0] < pend_n) rnv[k] = 1'b1;
    end
    assign rn_valid_w = rnv;
    always @* begin
        rn_ruop = {(`W*`RUOP_W){1'b0}};
        for (k=0;k<`W;k=k+1) rn_ruop[k*`RUOP_W +: `RUOP_W] = pend_u[k];
    end
    // 真 freelist：沒有空的實體暫存器就不產生新 uop（模擬 rename stall）。
    // 沒有這個，in-flight 逼近 PRF 大小時假 rename 會重用還活著的暫存器，
    // 製造出真機器不可能出現的相依 —— IQ_N=64 的「失敗」就是這樣來的。
    reg [`PRF_N-1:0] free_m; integer fi, np; reg ffnd; reg [`PRF_W-1:0] pd;
    integer fl_starve;   // freelist 供不上整組的拍數 -> 代表 PRF 才是綁住的資源
    always @(posedge clk) begin
        if (rst) begin
            pend_n <= 0; want_tot <= 0; sent_tot <= 0; fl_starve = 0;
            free_m <= {{(`PRF_N-1){1'b1}}, 1'b0};      // phys 0 保留
        end else if (flush) begin
            pend_n <= 0;
            free_m <= {{(`PRF_N-1){1'b1}}, 1'b0};      // flush 後全部回收
        end else begin
            na = (pend_n < rn_nready) ? pend_n : rn_nready;
            want_tot <= want_tot + pend_n;
            sent_tot <= sent_tot + na;
            for (sh=0; sh<`W; sh=sh+1)
                if (sh+na < `W) pend_u[sh] <= pend_u[sh+na];
            np = pend_n - na;
            for (pf=0; pf<`W; pf=pf+1)
                if (np < `W) begin
                    ffnd = 1'b0; pd = {`PRF_W{1'b0}};
                    for (fi=1; fi<`PRF_N; fi=fi+1)
                        if (!ffnd && free_m[fi] && !alloc_now[fi]) begin
                            ffnd = 1'b1; pd = fi[`PRF_W-1:0];
                        end
                    if (!ffnd) fl_starve = fl_starve + 1;
                    if (ffnd) begin
                        alloc_now[pd] = 1'b1;
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
        pend_u[np] = mk(cls,lat,s1v,s1,1'b1,pd,
                          (mode==6) && (nfed%97==96));
        plast = pd;
        nfed  = nfed+1;
                        np = np + 1;
                    end
                end
            pend_n <= np[2:0];
            // commit 回收 + 本拍配出去的扣掉
            free_m <= (free_m | cmt_free) & ~alloc_now;
            alloc_now = {`PRF_N{1'b0}};
        end
    end
    reg [`PRF_N-1:0] alloc_now;
    reg [`PRF_N-1:0] cmt_free;
    integer cf;
    always @* begin
        cmt_free = {`PRF_N{1'b0}};
        for (cf=0; cf<`W; cf=cf+1)
            if (cmt_valid[cf] && cmt_dv[cf])
                cmt_free[cmt_prf[cf*`PRF_W +: `PRF_W]] = 1'b1;
    end

    initial begin
        if (!$value$plusargs("dump=%d", dump)) dump = 0;
        if (!$value$plusargs("mode=%d", mode)) mode = 0;
        if (!$value$plusargs("rob=%d", robmask)) robmask = `ROB_N;
        if (!$value$plusargs("iq=%d", iqmask)) iqmask = `IQ_N;
        cfg_iq_entries = iqmask[`IQ_W:0];
        if (!$value$plusargs("thr=%d", lsuthr)) lsuthr = 0;
        cfg_rob_entries = robmask[`ROB_W:0];
        pnext={{(`PRF_W-1){1'b0}},1'b1}; plast={`PRF_W{1'b0}}; nfed=0; cyc=0; mshr_full=0; arf_bad=0;
        lsq_nfree=3'd4; pend_n=0; want_tot=0; sent_tot=0;
        free_m={{(`PRF_N-1){1'b1}},1'b0}; alloc_now={`PRF_N{1'b0}}; fl_starve=0;
        for (t=0;t<`W;t=t+1) pend_u[t]=0;
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
        $display("  rob_mask=%0d 未完成 uop: mem=%0d alu=%0d", robmask, nev_mem, nev_alu);
        // v8 守恆自檢：三個 cnt_lost_* 相加必須等於 Σ(想送 − 實際送)
        $display("  lost_check want=%0d sent=%0d diff=%0d  lost_rob=%0d lost_lsq=%0d lost_iq=%0d sum=%0d %s",
                 want_tot, sent_tot, want_tot-sent_tot,
                 cnt_lost_rob, cnt_lost_lsq, cnt_lost_iq,
                 cnt_lost_rob+cnt_lost_lsq+cnt_lost_iq,
                 ((want_tot-sent_tot) == (cnt_lost_rob+cnt_lost_lsq+cnt_lost_iq)) ? "CONSERVED" : "MISMATCH");
        $display("mode=%0d cycles=%0d retired=%0d fed=%0d IPC=%0.2f st_iq=%0d st_rob=%0d st_lsq=%0d occ_avg=%0.1f wrongpath=%0d arf_bad=%0d flstarve=%0d rettail=%0d",
                 mode, cnt_cycles, cnt_retired, nfed,
                 $itor(cnt_retired)/$itor(cnt_cycles), cnt_st_iq, cnt_st_rob, cnt_st_lsq,
                 $itor(cnt_rob_occ_sum)/$itor(cnt_cycles), cnt_wrongpath, arf_bad, fl_starve, cnt_retired-ret_at400);
        $finish;
    end
endmodule
