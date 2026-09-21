`include "common/ifc.vh"
// ===================================================================
// Agent E — be_eu（EX/WB 級）
//   EU 只做延遲倒數，沒有任何運算（PLAN §5.4）。
//
//   ---- v4：完成輪 -> ROB-index 定址的 in-flight pool（修容量 bug）----
//   每條 issue 出去的 uop 直接拿自己的 robidx 當 slot 編號。
//   ROB entry 在 commit 之前不會被重新配置、每條 uop 只會被 issue 一次，
//   所以「兩條 in-flight uop 撞到同一個 slot」在結構上不可能發生：
//   **不需要配置邏輯、不需要空槽搜尋、也不可能溢位丟件。**
//   pool 大小直接由 `ROB_N 推導，ROB 加大時自動跟上。
//
//   舊版是 4 lane x 16 槽的環形輪（容量固定 64）：ROB mask 開到 96/128
//   之後 in-flight 數會超過輪子容量，找不到空槽就把 uop 丟掉，那條 uop
//   永遠不會 writeback -> ROB head 永久卡死 -> commit 停擺。
//   這是「容量不足」不是「寬度不符」，lint / yosys 都抓不到，而且
//   mask 只開到 64 時完全正常。實測掉件數 0 / 4 / 5 @ mask 64 / 96 / 128，
//   只要 4 件就足以讓 IPC 從 1.391 崩到 0.072。
//
//   每個 slot：v + wt(等 LSU) + dv + cnt(`LAT_W) + prf(`PRF_W)
//     cnt 倒數到 0 = 可寫回；搶不到 writeback port 就停在 0 等下一拍，
//     不會被丟掉也不需要重新排程（舊版的「重排」正是殘留量爆掉的原因）。
//     lat<=1 的 uop 與本拍回來的 lsu_done 走 bypass 直接參加本拍競爭，
//     時序與舊版一致：lat=1 的相依者下一拍就能 issue（back-to-back）。
//     writeback port 用旋轉優先權挑，不會餓死。
//
//   UC_LOAD / UC_STORE / UC_AMO 不在這裡完成：slot 標成 wt，
//   用 rq_pend 位元（一樣由 robidx 定址，同樣不可能溢位）送 lsu_req_*，
//   等 lsu_done_* 回來清掉 wt 才參加 writeback。
//
//   prf_ready scoreboard：issue 且 (mem 或 lat>=2) -> clear；writeback -> set；
//     同拍撞同一個 bit 時 clear 優先。lat<=1 刻意不 clear（set/clear 同邊緣）。
//
//   建模規則：欄位 <= 8 bit；單 clock 同步；只用常數索引展開的 mux 鏈
//             （無 $mem / $shift / $shiftx / $mul）；全 if/else 無 case。
// ===================================================================
/* verilator lint_off MULTITOP */   // 四個模組一起 lint 時本來就沒有唯一 top
module be_eu (
    input  wire clk, input wire rst, input wire flush,
    input  wire [`W-1:0]         iss_valid,
    input  wire [`W*`RUOP_W-1:0] iss_ruop,
    input  wire [`W*`ROB_W-1:0]  iss_robidx,
    output wire [`W-1:0]         lsu_req_v,
    output wire [`W*`ROB_W-1:0]  lsu_req_rob,
    input  wire                  lsu_ready,
    input  wire [`W-1:0]         lsu_done_v,
    input  wire [`W*`ROB_W-1:0]  lsu_done_rob,
    input  wire [`W*`PRF_W-1:0]  lsu_done_prf,
    output wire [`W-1:0]         wb_valid,
    output wire [`W*`PRF_W-1:0]  wb_prf,
    output wire [`W*`ROB_W-1:0]  wb_robidx,
    output wire [`PRF_N-1:0]     prf_ready
);
    localparam [`LAT_W-1:0] LAT1 = {{(`LAT_W-1){1'b0}}, 1'b1};
    localparam [`LAT_W-1:0] LAT2 = {{(`LAT_W-2){1'b0}}, 2'b10};

    // ---------------------------------------------- 狀態（全部由 `ROB_N 推導）
    reg [`ROB_N-1:0]        eu_v;       // slot 佔用（index = robidx）
    reg [`ROB_N-1:0]        eu_wt;      // 等 LSU 回來
    reg [`ROB_N-1:0]        eu_dv;      // 有沒有實體 dst
    reg [`ROB_N*`LAT_W-1:0] eu_cnt;     // 剩餘延遲
    reg [`ROB_N*`PRF_W-1:0] eu_prf;     // 目的實體暫存器
    reg [`ROB_N-1:0]        rq_pend;    // 還沒送出去的 LSU request
    reg [`ROB_W-1:0]        rr_wb, rr_rq;   // 旋轉優先權基準
    reg [`PRF_N-1:0]        prf_rdy;
    reg [`W-1:0]            r_wb_v,  r_req_v;
    reg [`W*`PRF_W-1:0]     r_wb_prf;
    reg [`W*`ROB_W-1:0]     r_wb_rob, r_req_rob;

    // ---------------------------------------------- 每 lane 的 issue 資訊
    wire [`RUOP_W-1:0] uop [0:`W-1];
    wire [`ROB_W-1:0]  irb [0:`W-1];
    wire [`ROB_W-1:0]  drb [0:`W-1];
    wire [`W-1:0]      i_mem, i_dv, i_now, i_pool;
    genvar g;
    generate for (g = 0; g < `W; g = g + 1) begin : lane
        assign uop[g]    = iss_ruop[g*`RUOP_W +: `RUOP_W];
        assign irb[g]    = iss_robidx[g*`ROB_W +: `ROB_W];
        assign drb[g]    = lsu_done_rob[g*`ROB_W +: `ROB_W];
        assign i_mem[g]  = (uop[g][`RUOP_CLASS] == `UC_LOAD)
                         | (uop[g][`RUOP_CLASS] == `UC_STORE)
                         | (uop[g][`RUOP_CLASS] == `UC_AMO);
        assign i_dv[g]   = uop[g][`RUOP_DV];
        // lat<=1 且非 mem：本拍就能參加 writeback 競爭（bypass）
        assign i_now[g]  = iss_valid[g] & ~i_mem[g] & (uop[g][`RUOP_LAT] <= LAT1);
        assign i_pool[g] = iss_valid[g] & (i_mem[g] | (uop[g][`RUOP_LAT] > LAT1));
    end endgenerate

    // ---------------------------------------------- 組合暫存
    reg [`ROB_N-1:0]    rdy, rot, pick_m, sent_m, mem_oh;
    reg [`W-1:0]        pick_v, sent_v;
    reg [`W*`ROB_W-1:0] pick_i, sent_i;
    reg [`W*`PRF_W-1:0] n_wb_prf;
    reg [`W*`ROB_W-1:0] n_wb_rob;
    reg [`PRF_N-1:0]    clr_m, set_m;
    reg [`ROB_N-1:0]    rot2;
    reg [`ROB_W-1:0]    off, pidx, off2, pidx2;
    reg [`PRF_W-1:0]    prfx;
    reg                 fnd, fnd2, dvx;
    integer             i, k, r, i2, r2;

    function [`ROB_N-1:0] ohr; input [`ROB_W-1:0] ix; integer q; begin
        ohr = {`ROB_N{1'b0}};
        for (q = 0; q < `ROB_N; q = q + 1) if (q[`ROB_W-1:0] == ix) ohr[q] = 1'b1;
    end endfunction
    function [`PRF_N-1:0] ohp; input [`PRF_W-1:0] ix; integer q; begin
        ohp = {`PRF_N{1'b0}};
        for (q = 0; q < `PRF_N; q = q + 1) if (q[`PRF_W-1:0] == ix) ohp[q] = 1'b1;
    end endfunction
    // 右旋：`ROB_W 段常數位移（常數位移是接線，不會生 $shift/$shiftx）
    function [`ROB_N-1:0] rotr; input [`ROB_N-1:0] v; input [`ROB_W-1:0] a;
        integer s; reg [`ROB_N-1:0] t; begin
        t = v;
        for (s = 0; s < `ROB_W; s = s + 1)
            if (a[s]) t = (t >> (1<<s)) | (t << (`ROB_N - (1<<s)));
        rotr = t;
    end endfunction

    assign wb_valid    = r_wb_v;
    assign wb_prf      = r_wb_prf;
    assign wb_robidx   = r_wb_rob;
    assign prf_ready   = prf_rdy;
    assign lsu_req_v   = r_req_v;
    assign lsu_req_rob = r_req_rob;

    // ---------------------------------------------- 可寫回集合 + writeback 挑選
    always @* begin
        // 1) 倒數到 0 且不在等 LSU 的 slot
        rdy = {`ROB_N{1'b0}};
        for (i = 0; i < `ROB_N; i = i + 1)
            if (eu_v[i] & ~eu_wt[i]
                & (eu_cnt[i*`LAT_W +: `LAT_W] == {`LAT_W{1'b0}})) rdy[i] = 1'b1;
        // 2) bypass：本拍 issue 的 lat<=1、本拍回來的 lsu_done
        for (k = 0; k < `W; k = k + 1) begin
            if (i_now[k])      rdy = rdy | ohr(irb[k]);
            // lsu_done 只在該 slot 真的佔用中且真的在等 LSU 時才算數：
            // flush 前後若有殘留的 done 回來，不會憑空生出一筆 writeback
            if (lsu_done_v[k]) rdy = rdy | (ohr(drb[k]) & eu_v & eu_wt);
        end

        // 3) 旋轉優先權挑最多 W 條
        rot    = rotr(rdy, rr_wb);
        pick_v = {`W{1'b0}};
        pick_m = {`ROB_N{1'b0}};
        pick_i = {(`W*`ROB_W){1'b0}};
        off    = {`ROB_W{1'b0}};
        pidx   = {`ROB_W{1'b0}};
        fnd    = 1'b0;
        for (r = 0; r < `W; r = r + 1) begin
            fnd = 1'b0;
            off = {`ROB_W{1'b0}};
            for (i = 0; i < `ROB_N; i = i + 1)
                if (!fnd && rot[i]) begin fnd = 1'b1; off = i[`ROB_W-1:0]; end
            if (fnd) begin
                pidx = rr_wb + off;                 // `ROB_N 是 2 的冪，自然折返
                pick_v[r] = 1'b1;
                pick_i[r*`ROB_W +: `ROB_W] = pidx;
                pick_m = pick_m | ohr(pidx);
                rot    = rot & ~ohr(off);
            end
        end

        // 4) 讀出被挑中 slot 的 dv/prf（常數索引展開；本拍 bypass 的走 issue 資料）
        n_wb_prf = {(`W*`PRF_W){1'b0}};
        n_wb_rob = {(`W*`ROB_W){1'b0}};
        set_m    = {`PRF_N{1'b0}};
        for (r = 0; r < `W; r = r + 1) begin
            pidx = pick_i[r*`ROB_W +: `ROB_W];
            dvx  = 1'b0;
            prfx = {`PRF_W{1'b0}};
            for (i = 0; i < `ROB_N; i = i + 1)
                if (i[`ROB_W-1:0] == pidx) begin
                    dvx  = eu_dv[i];
                    prfx = eu_prf[i*`PRF_W +: `PRF_W];
                end
            for (k = 0; k < `W; k = k + 1)
                if (i_now[k] && (irb[k] == pidx)) begin
                    dvx  = i_dv[k];
                    prfx = uop[k][`RUOP_D];
                end
            if (pick_v[r]) begin
                n_wb_prf[r*`PRF_W +: `PRF_W] = prfx;
                n_wb_rob[r*`ROB_W +: `ROB_W] = pidx;
                if (dvx) set_m = set_m | ohp(prfx);
            end
        end

        // 5) issue 當拍清掉 dst 的 ready（lat<=1 不清，見檔頭）
        clr_m  = {`PRF_N{1'b0}};
        mem_oh = {`ROB_N{1'b0}};
        for (k = 0; k < `W; k = k + 1) begin
            if (iss_valid[k] & i_dv[k] & (i_mem[k] | (uop[k][`RUOP_LAT] >= LAT2)))
                clr_m = clr_m | ohp(uop[k][`RUOP_D]);
            if (iss_valid[k] & i_mem[k]) mem_oh = mem_oh | ohr(irb[k]);
        end
    end

    // ---------------------------------------------- LSU request 挑選
    always @* begin
        rot2    = rotr(rq_pend, rr_rq);
        sent_v = {`W{1'b0}};
        sent_m = {`ROB_N{1'b0}};
        sent_i = {(`W*`ROB_W){1'b0}};
        off2    = {`ROB_W{1'b0}};
        pidx2   = {`ROB_W{1'b0}};
        fnd2    = 1'b0;
        if (lsu_ready)
            for (r2 = 0; r2 < `W; r2 = r2 + 1) begin
                fnd2 = 1'b0;
                off2 = {`ROB_W{1'b0}};
                for (i2 = 0; i2 < `ROB_N; i2 = i2 + 1)
                    if (!fnd2 && rot2[i2]) begin fnd2 = 1'b1; off2 = i2[`ROB_W-1:0]; end
                if (fnd2) begin
                    pidx2 = rr_rq + off2;
                    sent_v[r2] = 1'b1;
                    sent_i[r2*`ROB_W +: `ROB_W] = pidx2;
                    sent_m = sent_m | ohr(pidx2);
                    rot2    = rot2 & ~ohr(off2);
                end
            end
    end

    // ---------------------------------------------- 時序
    always @(posedge clk) begin
        if (rst || flush) begin
            eu_v      <= {`ROB_N{1'b0}};
            eu_wt     <= {`ROB_N{1'b0}};
            eu_dv     <= {`ROB_N{1'b0}};
            eu_cnt    <= {(`ROB_N*`LAT_W){1'b0}};
            eu_prf    <= {(`ROB_N*`PRF_W){1'b0}};
            rq_pend   <= {`ROB_N{1'b0}};
            rr_wb     <= {`ROB_W{1'b0}};
            rr_rq     <= {`ROB_W{1'b0}};
            prf_rdy   <= {`PRF_N{1'b1}};    // 管線清空，所有暫存器視為就緒
            r_wb_v    <= {`W{1'b0}};
            r_wb_prf  <= {(`W*`PRF_W){1'b0}};
            r_wb_rob  <= {(`W*`ROB_W){1'b0}};
            r_req_v   <= {`W{1'b0}};
            r_req_rob <= {(`W*`ROB_W){1'b0}};
        end else begin
            r_wb_v    <= pick_v;
            r_wb_prf  <= n_wb_prf;
            r_wb_rob  <= n_wb_rob;
            r_req_v   <= sent_v;
            r_req_rob <= sent_i;
            prf_rdy   <= (prf_rdy | set_m) & ~clr_m;        // clear 優先
            rr_wb     <= rr_wb + {{(`ROB_W-1){1'b0}}, 1'b1};
            rr_rq     <= rr_rq + {{(`ROB_W-1){1'b0}}, 1'b1};
            rq_pend   <= (rq_pend | mem_oh) & ~sent_m;

            for (i = 0; i < `ROB_N; i = i + 1) begin
                // 預設：倒數（只有還在等的才動）
                if (eu_v[i] & ~eu_wt[i]
                    & (eu_cnt[i*`LAT_W +: `LAT_W] != {`LAT_W{1'b0}}))
                    eu_cnt[i*`LAT_W +: `LAT_W]
                        <= eu_cnt[i*`LAT_W +: `LAT_W] - LAT1;
                // lsu_done 回來：解除等待
                for (k = 0; k < `W; k = k + 1)
                    if (lsu_done_v[k] && (drb[k] == i[`ROB_W-1:0])) eu_wt[i] <= 1'b0;
                // 被挑中寫回：釋放 slot
                if (pick_m[i]) eu_v[i] <= 1'b0;
                // 本拍 issue：用自己的 robidx 當 slot（優先權最高）
                for (k = 0; k < `W; k = k + 1)
                    if (iss_valid[k] && (irb[k] == i[`ROB_W-1:0])) begin
                        // lat<=1 且本拍就被挑中寫回的，根本不用進 pool
                        if (i_pool[k] || !pick_m[i]) begin
                            eu_v[i]   <= 1'b1;
                            eu_wt[i]  <= i_mem[k];
                            eu_dv[i]  <= i_dv[k];
                            eu_prf[i*`PRF_W +: `PRF_W] <= uop[k][`RUOP_D];
                            eu_cnt[i*`LAT_W +: `LAT_W] <=
                                (uop[k][`RUOP_LAT] >= LAT2) ? (uop[k][`RUOP_LAT] - LAT2)
                                                            : {`LAT_W{1'b0}};
                        end else begin
                            eu_v[i] <= 1'b0;
                        end
                    end
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    // lsu_done_prf 不用：EU 自己在 issue 時就把 dst 存進 slot 了，
    // 用自己的比相信 LSU 回傳的更穩（不依賴對方有沒有正確回填）。
    wire _unused_be_eu = &{1'b0, iss_ruop, lsu_done_prf, 1'b0};
    /* verilator lint_on UNUSEDSIGNAL */
endmodule
