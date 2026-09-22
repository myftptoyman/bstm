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
    input  wire [`IQ_W:0]            cfg_iq_entries,
    input  wire [2:0]            cfg_issue_width,
    input  wire [`W-1:0]         ds_valid,
    input  wire [`W*`RUOP_W-1:0] ds_ruop,
    input  wire [`W*`ROB_W-1:0]  ds_robidx,
    output wire [2:0]            ds_nready,    // v8: 這拍能收前幾條（0..`W）
    input  wire [`PRF_N-1:0]     prf_ready,
    output wire [`W-1:0]         iss_valid,
    output wire [`W*`RUOP_W-1:0] iss_ruop,
    output wire [`W*`ROB_W-1:0]  iss_robidx,
    output wire [47:0]           cnt_st_iq
);
    // 序號寬度 = `ROB_W+1：見檔頭「繞回安全性」的證明
    localparam SW = `ROB_W + 1;
    localparam TN = 1 << `IQ_W;    // 錦標賽樹要 2 的冪，IQ_N 不足的部分補無效

    // ------------------------------------------------ 狀態
    reg [`IQ_N-1:0]   e_v;
    // 全部用 packed vector：unpacked 陣列在 IQ_N=64 時 verilator 無法展開
    // for 迴圈內的非阻塞賦值（BLKLOOPINIT），packed 則沒有這個限制。
    reg [`IQ_N*`RUOP_W-1:0] e_uop;
    reg [`IQ_N*`ROB_W-1:0]  e_rob;
    reg [`IQ_N*`PRF_N-1:0]  e_pend;         // wakeup bit-matrix（每列一條 entry）
    reg [`IQ_N*SW-1:0]      e_seq;          // 配置序號（取代 IQ_N x IQ_N 的 age matrix）
    reg [SW-1:0]            seq_ctr;
    reg [`PRF_N-1:0]  wait_m;               // dst 已配置、producer 尚未 issue
    reg [47:0]        c_st_iq;

    // ------------------------------------------------ 組合暫存
    reg [`IQ_N-1:0]   cfg_m, rdy_m, div_m, mul_m, sel_m, elig, avail, av;
    reg [`W-1:0]      s_v, a_ok;
    reg [`W*`IQ_W-1:0] a_slot;   // 每 lane 實際用的 IQ 槽
    reg [`W-1:0]      a_okv;     // 該 lane 真的有槽可用（防呆：協定已保證）
    reg [2:0]         n_free;
    reg [1:0]         pcnt;
    reg [`W*`RUOP_W-1:0] s_uop;
    reg [`W*`ROB_W-1:0]  s_rob;
    reg [`W*`IQ_W-1:0]   a_idx;
    reg [`W*SW-1:0]      seq_new;
    reg [SW-1:0]         seq_nxt, scnt;
    reg [TN-1:0]         t_v;
    reg [TN*`IQ_W-1:0]   t_i;
    reg [TN*SW-1:0]      t_s;
    integer lv;
    reg [`W*`PRF_N-1:0]  pend_new;
    reg [`PRF_N-1:0]  iss_dst, dst_disp, src_t;
    reg [`RUOP_W-1:0] u_t;
    /* verilator lint_off UNUSEDSIGNAL */
    reg [`RUOP_W-1:0] v_t, w_t;     // 只取 DV/D/S1/S2 欄位
    /* verilator lint_on UNUSEDSIGNAL */
    reg [`IQ_W-1:0]   pick, apick;
    reg               fnd, afnd, div_used;
    reg [1:0]         mul_cnt;
    integer si, sr, aj, ar, br, bj, wi, wk;

    wire [`PRF_N-1:0] eff_ready = prf_ready & ~wait_m;

    // 讀陣列：全部走常數索引展開的 mux 鏈（規則 3）
    function [`RUOP_W-1:0] rd_uop; input [`IQ_W-1:0] ix; integer q; begin
        rd_uop = {`RUOP_W{1'b0}};
        for (q = 0; q < `IQ_N; q = q + 1)
            if (q[`IQ_W-1:0] == ix) rd_uop = e_uop[q*`RUOP_W +: `RUOP_W];
    end endfunction
    function [`ROB_W-1:0] rd_rob; input [`IQ_W-1:0] ix; integer q; begin
        rd_rob = {`ROB_W{1'b0}};
        for (q = 0; q < `IQ_N; q = q + 1)
            if (q[`IQ_W-1:0] == ix) rd_rob = e_rob[q*`ROB_W +: `ROB_W];
    end endfunction
    // 繞回安全的「a 比 b 老」：差值的最高位（見檔頭證明，活著的 entry 序號差 < 2^(SW-1)）
    function older; input [SW-1:0] a; input [SW-1:0] b; reg [SW-1:0] d; begin
        d = a - b;
        older = d[SW-1];
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
            u_t = e_uop[si*`RUOP_W +: `RUOP_W];
            if (e_v[si] && ((e_pend[si*`PRF_N +: `PRF_N] & ~eff_ready) == {`PRF_N{1'b0}}))
                rdy_m[si] = 1'b1;
            if (e_v[si] && (u_t[`RUOP_CLASS +: `UC_W] == `UC_DIV)) div_m[si] = 1'b1;
            if (e_v[si] && (u_t[`RUOP_CLASS +: `UC_W] == `UC_MUL)) mul_m[si] = 1'b1;
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
            // oldest-first：平衡錦標賽樹（深度 `IQ_W，取代 IQ_N 深的線性比較鏈）
            for (si = 0; si < TN; si = si + 1) begin
                t_v[si]              = (si < `IQ_N) ? elig[si] : 1'b0;
                t_i[si*`IQ_W +: `IQ_W] = si[`IQ_W-1:0];
                t_s[si*SW +: SW]     = (si < `IQ_N) ? e_seq[si*SW +: SW] : {SW{1'b0}};
            end
            for (lv = 0; lv < `IQ_W; lv = lv + 1)
                for (si = 0; si < TN; si = si + (2 << lv))
                    if (!t_v[si] ||
                        (t_v[si + (1<<lv)] &&
                         older(t_s[(si + (1<<lv))*SW +: SW], t_s[si*SW +: SW]))) begin
                        t_v[si]                = t_v[si + (1<<lv)];
                        t_i[si*`IQ_W +: `IQ_W] = t_i[(si + (1<<lv))*`IQ_W +: `IQ_W];
                        t_s[si*SW +: SW]       = t_s[(si + (1<<lv))*SW +: SW];
                    end
            fnd  = t_v[0];
            pick = t_i[0 +: `IQ_W];
            if (fnd && (sr[2:0] < cfg_issue_width)) begin
                u_t = rd_uop(pick);
                sel_m   = sel_m | oh32(pick);
                s_v[sr] = 1'b1;
                s_uop[sr*`RUOP_W +: `RUOP_W] = u_t;
                s_rob[sr*`ROB_W  +: `ROB_W ] = rd_rob(pick);
                if (u_t[`RUOP_DV]) iss_dst = iss_dst | oh64(u_t[`RUOP_D +: `PRF_W]);
                if ((u_t[`RUOP_CLASS +: `UC_W] == `UC_DIV)) div_used = 1'b1;
                if ((u_t[`RUOP_CLASS +: `UC_W] == `UC_MUL)) mul_cnt  = mul_cnt + 2'd1;
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
        // v8：回報可收條數（只看自己的空槽，不看 ds_valid -> 不會有組合迴圈）
        n_free = {2'b0, a_ok[0]} + {2'b0, a_ok[1]} + {2'b0, a_ok[2]} + {2'b0, a_ok[3]};

    end

    // 第二段：只有這裡會讀 ds_valid/ds_ruop。跟上面分開寫是必要的 ——
    // 合在同一個 always @* 裡，verilator 會把 ds_valid -> a_ok 當成組合迴路
    // （ds_ready = &a_ok -> be_dispatch -> ds_valid），實際上 a_ok 不依賴 ds_valid。
    always @* begin
        // v8 partial accept：第 j 條 valid 的 uop 進第 j 個空槽（依序、不跳號）
        pcnt   = 2'd0;
        a_slot = {(`W*`IQ_W){1'b0}};
        a_okv  = {`W{1'b0}};
        for (br = 0; br < `W; br = br + 1) begin
            for (bj = 0; bj < `W; bj = bj + 1)
                if (bj[1:0] == pcnt) begin
                    a_slot[br*`IQ_W +: `IQ_W] = a_idx[bj*`IQ_W +: `IQ_W];
                    a_okv[br] = a_ok[bj];        // 第 pcnt 個空槽真的存在嗎
                end
            if (ds_valid[br]) pcnt = pcnt + 2'd1;
        end

        // 本拍 dispatch 的 dst 先算進「未就緒」，同拍 lane0->lane3 的相依才會對
        dst_disp = {`PRF_N{1'b0}};
        for (br = 0; br < `W; br = br + 1) begin
            w_t = ds_ruop[br*`RUOP_W +: `RUOP_W];
            if (ds_valid[br] && w_t[`RUOP_DV])
                dst_disp = dst_disp | oh64(w_t[`RUOP_D +: `PRF_W]);
        end

        // 新 entry 的 pend 遮罩與配置序號（同拍 lane0->lane3 依序遞增 = 程式順序）
        pend_new = {(`W*`PRF_N){1'b0}};
        seq_new  = {(`W*SW){1'b0}};
        scnt     = seq_ctr;
        for (br = 0; br < `W; br = br + 1) begin
            w_t   = ds_ruop[br*`RUOP_W +: `RUOP_W];
            src_t = {`PRF_N{1'b0}};
            if (w_t[`RUOP_S1V]) src_t = src_t | oh64(w_t[`RUOP_S1 +: `PRF_W]);
            if (w_t[`RUOP_S2V]) src_t = src_t | oh64(w_t[`RUOP_S2 +: `PRF_W]);
            pend_new[br*`PRF_N +: `PRF_N] = src_t & ~(eff_ready & ~dst_disp);
            seq_new [br*SW     +: SW     ] = scnt;
            if (ds_valid[br]) scnt = scnt + {{(SW-1){1'b0}}, 1'b1};
        end
        seq_nxt = scnt;
    end

    // 協定保證 popcount(ds_valid) <= ds_nready；再擋一次純粹是防呆，
    // 免得上游違約時變成「靜默蓋掉別人的 IQ entry」這種最難查的錯
    wire [`W-1:0] alloc_en = ds_valid & a_okv & {`W{~flush}};

    assign ds_nready  = n_free;         // v8: partial accept，剩幾格就收幾條
    assign iss_valid  = flush ? {`W{1'b0}} : s_v;
    assign iss_ruop   = s_uop;
    assign iss_robidx = s_rob;
    assign cnt_st_iq  = c_st_iq;

    // ------------------------------------------------ 時序
    always @(posedge clk) begin
        if (rst || flush) begin
            e_v     <= {`IQ_N{1'b0}};
            wait_m  <= {`PRF_N{1'b0}};
            e_uop   <= {(`IQ_N*`RUOP_W){1'b0}};
            e_rob   <= {(`IQ_N*`ROB_W){1'b0}};
            e_pend  <= {(`IQ_N*`PRF_N){1'b0}};
            e_seq   <= {(`IQ_N*SW){1'b0}};
            seq_ctr <= {SW{1'b0}};          // flush 後沒有活著的 entry，歸零安全
            if (rst) c_st_iq <= 48'd0;
        end else begin
            wait_m  <= (wait_m & ~iss_dst) | dst_disp;
            seq_ctr <= seq_nxt;
            for (wi = 0; wi < `IQ_N; wi = wi + 1) begin
                // 預設路徑：bit-matrix wakeup（age 欄不需要清了）
                if (sel_m[wi]) e_v[wi] <= 1'b0;
                e_pend[wi*`PRF_N +: `PRF_N] <= e_pend[wi*`PRF_N +: `PRF_N] & ~eff_ready;
                // 配置路徑（常數索引展開，lane 之間互斥）
                for (wk = 0; wk < `W; wk = wk + 1)
                    if (alloc_en[wk] && (a_slot[wk*`IQ_W +: `IQ_W] == wi[`IQ_W-1:0])) begin
                        e_v[wi]                     <= 1'b1;
                        e_uop[wi*`RUOP_W +: `RUOP_W] <= ds_ruop[wk*`RUOP_W +: `RUOP_W];
                        e_rob[wi*`ROB_W  +: `ROB_W ] <= ds_robidx[wk*`ROB_W +: `ROB_W];
                        e_pend[wi*`PRF_N +: `PRF_N] <= pend_new[wk*`PRF_N +: `PRF_N];
                        e_seq[wi*SW      +: SW     ] <= seq_new[wk*SW +: SW];
                    end
            end
        end
        if (rst)            c_st_iq <= 48'd0;
        else if (n_free == 3'd0) c_st_iq <= c_st_iq + 48'd1;   // v8: 真的一格都沒有才算
    end
endmodule
