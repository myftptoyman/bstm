`include "common/ifc.vh"
// ===================================================================
// Agent E — be_eu（EX/WB 級）
//   EU 只做延遲倒數，沒有任何運算（PLAN §5.4）。
//
//   完成輪（completion wheel）：每個 issue lane 一組 16 槽的環形表，
//   slot = (wh_ptr + lat - 1)。每拍 wh_ptr +1，落在 wh_ptr 的項目寫回。
//   lat<=1 的 uop 不進輪子，直接在 issue 當拍登記 writeback（下一拍輸出），
//   所以 lat=1 的相依者下一拍就能 issue（back-to-back）。
//
//   UC_LOAD / UC_STORE / UC_AMO 不在這裡完成：丟進 mem request queue，
//   經 lsu_req_* 給 LSU，等 lsu_done_* 回來才 writeback。
//
//   prf_ready scoreboard（PRF_N 位元）：
//     issue 且 (mem 或 lat>=2) -> clear；writeback -> set；
//     同拍 clear 與 set 撞同一個 bit 時 **clear 優先**（新的配置蓋掉舊結果）。
//     lat<=1 的 uop 刻意不 clear（它的 set 與 clear 會落在同一個邊緣）。
//
//   建模規則：欄位 <= 8 bit；單 clock 同步；陣列只用常數索引（展開 mux 鏈）；
//             全 if/else 無 case；無 initial/$display。
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
    localparam WHN = 16;                       // 完成輪槽數 = 2^LAT_W

    // ---------------------------------------------- 狀態
    reg [`W*WHN-1:0]     wh_v;                 // index = lane*16 + slot
    reg [`W*WHN-1:0]     wh_dv;
    reg [`PRF_W-1:0]     wh_prf [0:`W*WHN-1];
    reg [`ROB_W-1:0]     wh_rob [0:`W*WHN-1];
    reg [3:0]            wh_ptr;
    reg [`PRF_N-1:0]     prf_rdy;
    reg [`ROB_N-1:0]     mem_dv;               // 每個 ROB entry 的 mem uop 有無 dst
    reg [`W-1:0]         r_wb_v;
    reg [`W*`PRF_W-1:0]  r_wb_prf;
    reg [`W*`ROB_W-1:0]  r_wb_rob;
    // mem request queue（16 深，push/pop 各 W）
    reg [`ROB_W-1:0]     mq [0:15];
    reg [3:0]            mq_head, mq_tail;
    reg [4:0]            mq_cnt;
    reg [`W-1:0]         r_req_v;
    reg [`W*`ROB_W-1:0]  r_req_rob;

    // ---------------------------------------------- 組合暫存
    reg [`W-1:0]         ins1_v, ins2_v, push_v, mem_iss;
    reg [`W*4-1:0]       ins1_s, ins2_s;
    reg [`W*`PRF_W-1:0]  ins1_prf, ins2_prf;
    reg [`W*`ROB_W-1:0]  ins1_rob, ins2_rob;
    reg [`W-1:0]         ins1_dv, ins2_dv;
    reg [`PRF_N-1:0]     clr_m, set_m;
    reg [`ROB_N-1:0]     mdv_set, mdv_clr;
    reg [`W-1:0]         n_wb_v;
    reg [`W*`PRF_W-1:0]  n_wb_prf;
    reg [`W*`ROB_W-1:0]  n_wb_rob;
    reg [WHN-1:0]        freev;
    reg [3:0]            fslot, start2;
    reg [4:0]            ff;
    reg                  fs, pv, pdv, now_v, insn_v, lose_pop, lose_now, wbv, wbdv, is_mem;
    reg [`PRF_W-1:0]     pprf, wbprf;
    reg [`ROB_W-1:0]     prob, wbrob;
    /* verilator lint_off UNUSEDSIGNAL */
    reg [`RUOP_W-1:0]    u;             // 只取 CLASS/LAT/DV/D 欄位
    /* verilator lint_on UNUSEDSIGNAL */
    reg [`LAT_W-1:0]     lat;
    reg [`W*2-1:0]       push_pos;
    reg [2:0]            ppos, n_push, n_pop, n_push_e;
    integer              ek, es;
    integer              mi, mj, mk;

    // 16 槽空位搜尋：用 4 段常數 part-select 的右旋（不產生 $shift/$shiftx），
    // 旋轉後取最低位的 1，再加回起點。回傳 {found, slot}。
    function [4:0] find_free;
        input [WHN-1:0] fr; input [3:0] st;
        reg [WHN-1:0] r0, r1, r2, r3; reg f; reg [3:0] j; integer q;
        begin
            r0 = st[0] ? {fr[0],   fr[WHN-1:1]} : fr;
            r1 = st[1] ? {r0[1:0], r0[WHN-1:2]} : r0;
            r2 = st[2] ? {r1[3:0], r1[WHN-1:4]} : r1;
            r3 = st[3] ? {r2[7:0], r2[WHN-1:8]} : r2;
            f = 1'b0; j = 4'd0;
            for (q = 0; q < WHN; q = q + 1)
                if (!f && r3[q]) begin f = 1'b1; j = q[3:0]; end
            find_free = {f, st + j};
        end
    endfunction
    function [WHN-1:0] oh16; input [3:0] ix; integer q; begin
        oh16 = {WHN{1'b0}};
        for (q = 0; q < WHN; q = q + 1) if (q[3:0] == ix) oh16[q] = 1'b1;
    end endfunction
    function rd_mdv; input [`ROB_W-1:0] ix; integer q; begin
        rd_mdv = 1'b0;
        for (q = 0; q < `ROB_N; q = q + 1) if (q[`ROB_W-1:0] == ix) rd_mdv = mem_dv[q];
    end endfunction
    function [`PRF_N-1:0] oh64; input [`PRF_W-1:0] ix; integer q; begin
        oh64 = {`PRF_N{1'b0}};
        for (q = 0; q < `PRF_N; q = q + 1) if (q[`PRF_W-1:0] == ix) oh64[q] = 1'b1;
    end endfunction
    // PRF_W 與 ROB_W 不再相等（PRF 可 sweep），ROB one-hot 要自己一份
    function [`ROB_N-1:0] ohrob; input [`ROB_W-1:0] ix; integer q; begin
        ohrob = {`ROB_N{1'b0}};
        for (q = 0; q < `ROB_N; q = q + 1) if (q[`ROB_W-1:0] == ix) ohrob[q] = 1'b1;
    end endfunction

    assign wb_valid    = r_wb_v;
    assign wb_prf      = r_wb_prf;
    assign wb_robidx   = r_wb_rob;
    assign prf_ready   = prf_rdy;
    assign lsu_req_v   = r_req_v;
    assign lsu_req_rob = r_req_rob;

    // ---------------------------------------------- 每 lane 的倒數 / 仲裁
    always @* begin
        ins1_v = {`W{1'b0}};  ins2_v = {`W{1'b0}};
        ins1_s = {(`W*4){1'b0}};      ins2_s = {(`W*4){1'b0}};
        ins1_prf = {(`W*`PRF_W){1'b0}}; ins2_prf = {(`W*`PRF_W){1'b0}};
        ins1_rob = {(`W*`ROB_W){1'b0}}; ins2_rob = {(`W*`ROB_W){1'b0}};
        ins1_dv = {`W{1'b0}}; ins2_dv = {`W{1'b0}};
        n_wb_v = {`W{1'b0}};
        n_wb_prf = {(`W*`PRF_W){1'b0}};
        n_wb_rob = {(`W*`ROB_W){1'b0}};
        clr_m = {`PRF_N{1'b0}};
        set_m = {`PRF_N{1'b0}};
        mem_iss = {`W{1'b0}};
        push_v  = {`W{1'b0}};
        mdv_set = {`ROB_N{1'b0}};
        mdv_clr = {`ROB_N{1'b0}};
        freev = {WHN{1'b0}}; fslot = 4'd0; start2 = 4'd0; ff = 5'd0;
        fs = 1'b0; pv = 1'b0; pdv = 1'b0; pprf = {`PRF_W{1'b0}}; prob = {`ROB_W{1'b0}};
        now_v = 1'b0; insn_v = 1'b0; lose_pop = 1'b0; lose_now = 1'b0;
        wbv = 1'b0; wbdv = 1'b0; wbprf = {`PRF_W{1'b0}}; wbrob = {`ROB_W{1'b0}};
        is_mem = 1'b0; u = {`RUOP_W{1'b0}}; lat = {`LAT_W{1'b0}};

        for (ek = 0; ek < `W; ek = ek + 1) begin
            // ---- 讀出本拍要完成的輪子槽（常數索引展開成 mux 鏈）
            pv = 1'b0; pdv = 1'b0; pprf = {`PRF_W{1'b0}}; prob = {`ROB_W{1'b0}};
            for (es = 0; es < WHN; es = es + 1)
                if (es[3:0] == wh_ptr) begin
                    pv   = wh_v [ek*WHN + es];
                    pdv  = wh_dv[ek*WHN + es];
                    pprf = wh_prf[ek*WHN + es];
                    prob = wh_rob[ek*WHN + es];
                end
            // ---- 本拍 issue 的 uop
            u      = iss_ruop[ek*`RUOP_W +: `RUOP_W];
            lat    = u[`RUOP_LAT];
            is_mem = (u[`RUOP_CLASS] == `UC_LOAD) | (u[`RUOP_CLASS] == `UC_STORE)
                   | (u[`RUOP_CLASS] == `UC_AMO);
            now_v   = iss_valid[ek] & ~is_mem & (lat <= 4'd1);
            insn_v  = iss_valid[ek] & ~is_mem & (lat >= 4'd2);
            mem_iss[ek] = iss_valid[ek] & is_mem;
            if (iss_valid[ek] & is_mem) begin
                if (u[`RUOP_DV]) mdv_set = mdv_set | ohrob(iss_robidx[ek*`ROB_W +: `ROB_W]);
                else             mdv_clr = mdv_clr | ohrob(iss_robidx[ek*`ROB_W +: `ROB_W]);
            end
            if (iss_valid[ek] & u[`RUOP_DV] & (is_mem | (lat >= 4'd2)))
                clr_m = clr_m | oh64(u[`RUOP_D]);

            // ---- 空槽向量（本拍 pop 掉的槽算空）
            for (es = 0; es < WHN; es = es + 1)
                freev[es] = (~wh_v[ek*WHN + es]) | (es[3:0] == wh_ptr);

            // ---- writeback 仲裁：lsu_done > 輪子 pop > 本拍 lat<=1（輸掉的重排）
            lose_pop = 1'b0; lose_now = 1'b0;
            if (lsu_done_v[ek]) begin
                wbv   = 1'b1;
                wbprf = lsu_done_prf[ek*`PRF_W +: `PRF_W];
                wbrob = lsu_done_rob[ek*`ROB_W +: `ROB_W];
                wbdv  = rd_mdv(lsu_done_rob[ek*`ROB_W +: `ROB_W]);
                lose_pop = pv;
                lose_now = now_v;
            end else if (pv) begin
                wbv = 1'b1; wbprf = pprf; wbrob = prob; wbdv = pdv;
                lose_now = now_v;
            end else if (now_v) begin
                wbv   = 1'b1;
                wbprf = u[`RUOP_D];
                wbrob = iss_robidx[ek*`ROB_W +: `ROB_W];
                wbdv  = u[`RUOP_DV];
            end else begin
                wbv = 1'b0; wbdv = 1'b0;
                wbprf = {`PRF_W{1'b0}}; wbrob = {`ROB_W{1'b0}};
            end
            n_wb_v[ek] = wbv;
            n_wb_prf[ek*`PRF_W +: `PRF_W] = wbprf;
            n_wb_rob[ek*`ROB_W +: `ROB_W] = wbrob;
            if (wbv & wbdv) set_m = set_m | oh64(wbprf);

            // ---- 仲裁輸掉的 pop：排回輪子（從 ptr+1 找第一個空槽）
            if (lose_pop) begin
                ff = find_free(freev, wh_ptr + 4'd1);
                fs = ff[4]; fslot = ff[3:0];
                if (fs) begin
                    ins1_v[ek] = 1'b1;
                    ins1_s[ek*4 +: 4]        = fslot;
                    ins1_prf[ek*`PRF_W +: `PRF_W] = pprf;
                    ins1_rob[ek*`ROB_W +: `ROB_W] = prob;
                    ins1_dv[ek] = pdv;
                    freev = freev & ~oh16(fslot);
                end
            end
            // ---- 本拍要放進輪子的：lat>=2 的新 uop，或仲裁輸掉的 lat<=1
            if (lose_now | insn_v) begin
                if (lose_now) start2 = wh_ptr + 4'd1;
                else          start2 = wh_ptr + lat - 4'd1;
                ff = find_free(freev, start2);
                fs = ff[4]; fslot = ff[3:0];
                if (fs) begin
                    ins2_v[ek] = 1'b1;
                    ins2_s[ek*4 +: 4]             = fslot;
                    ins2_prf[ek*`PRF_W +: `PRF_W] = u[`RUOP_D];
                    ins2_rob[ek*`ROB_W +: `ROB_W] = iss_robidx[ek*`ROB_W +: `ROB_W];
                    ins2_dv[ek] = u[`RUOP_DV];
                    freev = freev & ~oh16(fslot);
                end
            end
        end

        // ---- mem request queue 的 push 位置（lane prefix）
        n_push = 3'd0;
        for (ek = 0; ek < `W; ek = ek + 1)
            if (mem_iss[ek]) n_push = n_push + 3'd1;
        n_pop = 3'd0;
        if (lsu_ready) begin
            if (mq_cnt >= 5'd4) n_pop = 3'd4;
            else                n_pop = mq_cnt[2:0];
        end
        // 佇列容量檢查（整組收或整組不收，見 README）
        for (ek = 0; ek < `W; ek = ek + 1)
            push_v[ek] = mem_iss[ek]
                       & ((mq_cnt - {2'b0, n_pop} + {2'b0, n_push}) <= 5'd16);
        ppos     = 3'd0;
        push_pos = {(`W*2){1'b0}};
        for (ek = 0; ek < `W; ek = ek + 1)
            if (push_v[ek]) begin
                push_pos[ek*2 +: 2] = ppos[1:0];
                ppos = ppos + 3'd1;
            end
        n_push_e = ppos;
    end

    // ---------------------------------------------- 時序
    always @(posedge clk) begin
        if (rst) begin
            wh_v    <= {(`W*WHN){1'b0}};
            wh_dv   <= {(`W*WHN){1'b0}};
            wh_ptr  <= 4'd0;
            prf_rdy <= {`PRF_N{1'b1}};      // 重置後所有實體暫存器視為就緒
            mem_dv  <= {`ROB_N{1'b0}};
            r_wb_v  <= {`W{1'b0}};
            r_wb_prf<= {(`W*`PRF_W){1'b0}};
            r_wb_rob<= {(`W*`ROB_W){1'b0}};
            mq_head <= 4'd0; mq_tail <= 4'd0; mq_cnt <= 5'd0;
            r_req_v <= {`W{1'b0}};
            r_req_rob <= {(`W*`ROB_W){1'b0}};
            for (mi = 0; mi < `W*WHN; mi = mi + 1) begin
                wh_prf[mi] <= {`PRF_W{1'b0}};
                wh_rob[mi] <= {`ROB_W{1'b0}};
            end
            for (mi = 0; mi < 16; mi = mi + 1) mq[mi] <= {`ROB_W{1'b0}};
        end else if (flush) begin
            wh_v    <= {(`W*WHN){1'b0}};
            wh_dv   <= {(`W*WHN){1'b0}};
            wh_ptr  <= 4'd0;
            prf_rdy <= {`PRF_N{1'b1}};      // 管線清空，所有暫存器重新視為就緒
            mem_dv  <= {`ROB_N{1'b0}};
            r_wb_v  <= {`W{1'b0}};
            mq_head <= 4'd0; mq_tail <= 4'd0; mq_cnt <= 5'd0;
            r_req_v <= {`W{1'b0}};
        end else begin
            wh_ptr   <= wh_ptr + 4'd1;
            r_wb_v   <= n_wb_v;
            r_wb_prf <= n_wb_prf;
            r_wb_rob <= n_wb_rob;
            prf_rdy  <= (prf_rdy | set_m) & ~clr_m;   // clear 優先

            // ---- 輪子：先清掉本拍 pop 的槽，再讓 insert 覆蓋（insert 在後，優先）
            for (ek = 0; ek < `W; ek = ek + 1)
                for (es = 0; es < WHN; es = es + 1) begin
                    if (es[3:0] == wh_ptr) wh_v[ek*WHN + es] <= 1'b0;
                    if (ins1_v[ek] && (ins1_s[ek*4 +: 4] == es[3:0])) begin
                        wh_v  [ek*WHN + es] <= 1'b1;
                        wh_dv [ek*WHN + es] <= ins1_dv[ek];
                        wh_prf[ek*WHN + es] <= ins1_prf[ek*`PRF_W +: `PRF_W];
                        wh_rob[ek*WHN + es] <= ins1_rob[ek*`ROB_W +: `ROB_W];
                    end
                    if (ins2_v[ek] && (ins2_s[ek*4 +: 4] == es[3:0])) begin
                        wh_v  [ek*WHN + es] <= 1'b1;
                        wh_dv [ek*WHN + es] <= ins2_dv[ek];
                        wh_prf[ek*WHN + es] <= ins2_prf[ek*`PRF_W +: `PRF_W];
                        wh_rob[ek*WHN + es] <= ins2_rob[ek*`ROB_W +: `ROB_W];
                    end
                end

            // ---- mem uop 的 dst 記錄（給 lsu_done 回來時判斷要不要 set prf_ready）
            mem_dv <= (mem_dv | mdv_set) & ~mdv_clr;

            // ---- mem request queue
            for (mj = 0; mj < `W; mj = mj + 1) begin
                r_req_v[mj] <= (mj[2:0] < n_pop);
                for (mi = 0; mi < 16; mi = mi + 1)
                    if (mi[3:0] == (mq_head + mj[3:0]))
                        r_req_rob[mj*`ROB_W +: `ROB_W] <= mq[mi];
            end
            for (mk = 0; mk < `W; mk = mk + 1)
                if (push_v[mk])
                    for (mi = 0; mi < 16; mi = mi + 1)
                        if (mi[3:0] == (mq_tail + {2'b0, push_pos[mk*2 +: 2]}))
                            mq[mi] <= iss_robidx[mk*`ROB_W +: `ROB_W];
            mq_head <= mq_head + {1'b0, n_pop};
            mq_tail <= mq_tail + {1'b0, n_push_e};
            mq_cnt  <= mq_cnt - {2'b0, n_pop} + {2'b0, n_push_e};
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    wire _unused_be_eu = &{1'b0, iss_ruop, 1'b0};   // S1/S2/WRONGPATH/MEMSTORE 等欄位本級不用
    /* verilator lint_on UNUSEDSIGNAL */
endmodule
