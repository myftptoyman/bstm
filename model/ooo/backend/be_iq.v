`include "common/ifc.vh"
// ===================================================================
// Agent E — be_iq（IS 級：wakeup + select）
//
//  wakeup：bit-matrix，不做 tag CAM
//    每個 entry 存一條 PRF_N(64) 位元的「尚未就緒來源」遮罩 e_pend[i]。
//    配置時 e_pend = (onehot(s1)|onehot(s2)) & ~eff_ready；
//    每拍 e_pend <= e_pend & ~eff_ready（整條 AND，bit-sliced 下就是幾個 word）。
//    就緒判斷 = ((e_pend[i] & ~eff_ready) == 0)，沒有任何 tag 比較器。
//
//  select：oldest-first，用 IQ_N x IQ_N age matrix
//    e_age[i][j] = 1 代表 entry j 比 i 老。
//    「最老的就緒 entry」= elig[i] && (e_age[i] & elig)==0（一定是 one-hot）。
//    每輪挑一條，挑完把它從 elig 拿掉再挑下一條，共 cfg_issue_width 輪。
//
//  資源限制：UC_DIV 同拍最多 1 條、UC_MUL 最多 2 條、總數 <= cfg_issue_width
//
//  建模規則：欄位 <= 8 bit（48-bit counter 除外）；單 clock 同步；
//            所有陣列只用常數索引（展開成 mux 鏈）-> 不會生 $mem / RAMGEM；
//            全 if/else，沒有 case。
// ===================================================================
/* verilator lint_off MULTITOP */   // 四個模組一起 lint 時本來就沒有唯一 top
module be_iq (
    input  wire clk, input wire rst, input wire flush,
    input  wire [5:0]            cfg_iq_entries,
    input  wire [2:0]            cfg_issue_width,
    input  wire [`W-1:0]         ds_valid,
    input  wire [`W*`RUOP_W-1:0] ds_ruop,
    input  wire [`W*`ROB_W-1:0]  ds_robidx,
    output wire                  ds_ready,
    input  wire [`PRF_N-1:0]     prf_ready,
    output wire [`W-1:0]         iss_valid,
    output wire [`W*`RUOP_W-1:0] iss_ruop,
    output wire [`W*`ROB_W-1:0]  iss_robidx,
    output wire [47:0]           cnt_st_iq
);
    // ------------------------------------------------ 狀態
    reg [`IQ_N-1:0]   e_v;
    reg [`RUOP_W-1:0] e_uop  [0:`IQ_N-1];
    reg [`ROB_W-1:0]  e_rob  [0:`IQ_N-1];
    reg [`PRF_N-1:0]  e_pend [0:`IQ_N-1];   // wakeup bit-matrix（每列一條 entry）
    reg [`IQ_N-1:0]   e_age  [0:`IQ_N-1];   // age matrix
    reg [`PRF_N-1:0]  wait_m;               // dst 已配置、producer 尚未 issue
    reg [47:0]        c_st_iq;

    // ------------------------------------------------ 組合暫存
    reg [`IQ_N-1:0]   cfg_m, rdy_m, div_m, mul_m, sel_m, elig, old1, avail, av, newer;
    reg [`W-1:0]      s_v, a_ok;
    reg [`W*`RUOP_W-1:0] s_uop;
    reg [`W*`ROB_W-1:0]  s_rob;
    reg [`W*`IQ_W-1:0]   a_idx;
    reg [`W*`IQ_N-1:0]   age_new;
    reg [`W*`PRF_N-1:0]  pend_new;
    reg [`PRF_N-1:0]  iss_dst, dst_disp, src_t;
    reg [`RUOP_W-1:0] u_t;
    /* verilator lint_off UNUSEDSIGNAL */
    reg [`RUOP_W-1:0] v_t, w_t;     // 只取 DV/D/S1/S2 欄位
    /* verilator lint_on UNUSEDSIGNAL */
    reg [`IQ_W-1:0]   pick, apick;
    reg               fnd, afnd, div_used;
    reg [1:0]         mul_cnt;
    integer si, sr, aj, ar, br, wi, wk;

    wire [`PRF_N-1:0] eff_ready = prf_ready & ~wait_m;

    // 讀陣列：全部走常數索引展開的 mux 鏈（規則 3）
    function [`RUOP_W-1:0] rd_uop; input [`IQ_W-1:0] ix; integer q; begin
        rd_uop = {`RUOP_W{1'b0}};
        for (q = 0; q < `IQ_N; q = q + 1) if (q[`IQ_W-1:0] == ix) rd_uop = e_uop[q];
    end endfunction
    function [`ROB_W-1:0] rd_rob; input [`IQ_W-1:0] ix; integer q; begin
        rd_rob = {`ROB_W{1'b0}};
        for (q = 0; q < `IQ_N; q = q + 1) if (q[`IQ_W-1:0] == ix) rd_rob = e_rob[q];
    end endfunction
    function [`PRF_N-1:0] oh64; input [`PRF_W-1:0] ix; integer q; begin
        oh64 = {`PRF_N{1'b0}};
        for (q = 0; q < `PRF_N; q = q + 1) if (q[`PRF_W-1:0] == ix) oh64[q] = 1'b1;
    end endfunction
    function [`IQ_N-1:0] oh32; input [`IQ_W-1:0] ix; integer q; begin
        oh32 = {`IQ_N{1'b0}};
        for (q = 0; q < `IQ_N; q = q + 1) if (q[`IQ_W-1:0] == ix) oh32[q] = 1'b1;
    end endfunction

    // ------------------------------------------------ wakeup + select
    always @* begin
        cfg_m = {`IQ_N{1'b0}};
        rdy_m = {`IQ_N{1'b0}};
        div_m = {`IQ_N{1'b0}};
        mul_m = {`IQ_N{1'b0}};
        for (si = 0; si < `IQ_N; si = si + 1) begin
            if ({1'b0, si[`IQ_W-1:0]} < cfg_iq_entries) cfg_m[si] = 1'b1;
            if (e_v[si] && ((e_pend[si] & ~eff_ready) == {`PRF_N{1'b0}})) rdy_m[si] = 1'b1;
            if (e_v[si] && (e_uop[si][`RUOP_CLASS] == `UC_DIV)) div_m[si] = 1'b1;
            if (e_v[si] && (e_uop[si][`RUOP_CLASS] == `UC_MUL)) mul_m[si] = 1'b1;
        end

        sel_m    = {`IQ_N{1'b0}};
        s_v      = {`W{1'b0}};
        s_uop    = {(`W*`RUOP_W){1'b0}};
        s_rob    = {(`W*`ROB_W){1'b0}};
        iss_dst  = {`PRF_N{1'b0}};
        div_used = 1'b0;
        mul_cnt  = 2'd0;
        u_t      = {`RUOP_W{1'b0}};
        for (sr = 0; sr < `W; sr = sr + 1) begin
            // 本輪可挑的 entry（扣掉已挑的與資源用完的類別）
            elig = {`IQ_N{1'b0}};
            for (si = 0; si < `IQ_N; si = si + 1)
                elig[si] = rdy_m[si] & ~sel_m[si]
                         & ~(div_used   & div_m[si])
                         & ~((mul_cnt >= 2'd2) & mul_m[si]);
            // oldest-first：沒有更老的 eligible entry
            old1 = {`IQ_N{1'b0}};
            for (si = 0; si < `IQ_N; si = si + 1)
                if (elig[si] && ((e_age[si] & elig) == {`IQ_N{1'b0}})) old1[si] = 1'b1;
            fnd  = 1'b0;
            pick = {`IQ_W{1'b0}};
            for (si = 0; si < `IQ_N; si = si + 1)
                if (old1[si] && !fnd) begin fnd = 1'b1; pick = si[`IQ_W-1:0]; end
            if (fnd && (sr[2:0] < cfg_issue_width)) begin
                u_t = rd_uop(pick);
                sel_m   = sel_m | oh32(pick);
                s_v[sr] = 1'b1;
                s_uop[sr*`RUOP_W +: `RUOP_W] = u_t;
                s_rob[sr*`ROB_W  +: `ROB_W ] = rd_rob(pick);
                if (u_t[`RUOP_DV]) iss_dst = iss_dst | oh64(u_t[`RUOP_D]);
                if ((u_t[`RUOP_CLASS] == `UC_DIV)) div_used = 1'b1;
                if ((u_t[`RUOP_CLASS] == `UC_MUL)) mul_cnt  = mul_cnt + 2'd1;
            end
        end
    end

    // ------------------------------------------------ 空槽配置 + age / pend 預算
    always @* begin
        avail = (~e_v | sel_m) & cfg_m;     // 本拍 issue 出去的槽可以馬上重用
        av    = avail;
        a_ok  = {`W{1'b0}};
        a_idx = {(`W*`IQ_W){1'b0}};
        for (ar = 0; ar < `W; ar = ar + 1) begin
            afnd  = 1'b0;
            apick = {`IQ_W{1'b0}};
            for (aj = 0; aj < `IQ_N; aj = aj + 1)
                if (av[aj] && !afnd) begin afnd = 1'b1; apick = aj[`IQ_W-1:0]; end
            a_ok[ar] = afnd;
            a_idx[ar*`IQ_W +: `IQ_W] = apick;
            if (afnd) av = av & ~oh32(apick);
        end

    end

    // 第二段：只有這裡會讀 ds_valid/ds_ruop。跟上面分開寫是必要的 ——
    // 合在同一個 always @* 裡，verilator 會把 ds_valid -> a_ok 當成組合迴路
    // （ds_ready = &a_ok -> be_dispatch -> ds_valid），實際上 a_ok 不依賴 ds_valid。
    always @* begin
        // 本拍 dispatch 的 dst 先算進「未就緒」，同拍 lane0->lane3 的相依才會對
        dst_disp = {`PRF_N{1'b0}};
        for (br = 0; br < `W; br = br + 1) begin
            w_t = ds_ruop[br*`RUOP_W +: `RUOP_W];
            if (ds_valid[br] && a_ok[br] && w_t[`RUOP_DV])
                dst_disp = dst_disp | oh64(w_t[`RUOP_D]);
        end

        // 新 entry 的 pend 遮罩與 age 列
        newer    = e_v & ~sel_m;
        pend_new = {(`W*`PRF_N){1'b0}};
        age_new  = {(`W*`IQ_N){1'b0}};
        for (br = 0; br < `W; br = br + 1) begin
            w_t   = ds_ruop[br*`RUOP_W +: `RUOP_W];
            src_t = {`PRF_N{1'b0}};
            if (w_t[`RUOP_S1V]) src_t = src_t | oh64(w_t[`RUOP_S1]);
            if (w_t[`RUOP_S2V]) src_t = src_t | oh64(w_t[`RUOP_S2]);
            pend_new[br*`PRF_N +: `PRF_N] = src_t & ~(eff_ready & ~dst_disp);
            age_new [br*`IQ_N  +: `IQ_N ] = newer;
            if (ds_valid[br] && a_ok[br])
                newer = newer | oh32(a_idx[br*`IQ_W +: `IQ_W]);
        end
    end

    wire [`W-1:0] alloc_en = ds_valid & a_ok & {`W{~flush}};

    assign ds_ready   = &a_ok;          // 只在有 W 個空槽時收一整組（all-or-nothing）
    assign iss_valid  = flush ? {`W{1'b0}} : s_v;
    assign iss_ruop   = s_uop;
    assign iss_robidx = s_rob;
    assign cnt_st_iq  = c_st_iq;

    // ------------------------------------------------ 時序
    always @(posedge clk) begin
        if (rst || flush) begin
            e_v    <= {`IQ_N{1'b0}};
            wait_m <= {`PRF_N{1'b0}};
            for (wi = 0; wi < `IQ_N; wi = wi + 1) begin
                e_uop[wi]  <= {`RUOP_W{1'b0}};
                e_rob[wi]  <= {`ROB_W{1'b0}};
                e_pend[wi] <= {`PRF_N{1'b0}};
                e_age[wi]  <= {`IQ_N{1'b0}};
            end
            if (rst) c_st_iq <= 48'd0;
        end else begin
            wait_m <= (wait_m & ~iss_dst) | dst_disp;
            for (wi = 0; wi < `IQ_N; wi = wi + 1) begin
                // 預設路徑：bit-matrix wakeup + 清掉被 issue 釋放的 age 欄
                if (sel_m[wi]) e_v[wi] <= 1'b0;
                e_pend[wi] <= e_pend[wi] & ~eff_ready;
                e_age[wi]  <= e_age[wi] & ~sel_m;
                // 配置路徑（常數索引展開，lane 之間互斥）
                for (wk = 0; wk < `W; wk = wk + 1)
                    if (alloc_en[wk] && (a_idx[wk*`IQ_W +: `IQ_W] == wi[`IQ_W-1:0])) begin
                        e_v[wi]    <= 1'b1;
                        e_uop[wi]  <= ds_ruop[wk*`RUOP_W +: `RUOP_W];
                        e_rob[wi]  <= ds_robidx[wk*`ROB_W +: `ROB_W];
                        e_pend[wi] <= pend_new[wk*`PRF_N +: `PRF_N];
                        e_age[wi]  <= age_new[wk*`IQ_N +: `IQ_N];
                    end
            end
        end
        if (rst)            c_st_iq <= 48'd0;
        else if (~ds_ready) c_st_iq <= c_st_iq + 48'd1;
    end
endmodule
