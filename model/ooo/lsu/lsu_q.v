`include "common/ifc.vh"
// ===================================================================
// BSTM OOO model — LSU: LDQ / STQ / MSHR            (Agent F)
//
// 本模組負責「離線算不出來的那一半」：MSHR 佔用與 memory-level
// parallelism (MLP)。cache 的 hit/miss 與無負載延遲是離線算好的，
// 經 fb_mem_event 餵進來（MEM_LEVEL = bit[1:0]，MEM_LAT_CLASS = bit[7:4]，
// 查 include/bstf.h 的 bstf_lat_table 得 base latency）。
//
// 結構（都是 MAX 尺寸 + cfg_* runtime mask）：
//   LDQ  16 entry  環狀佇列，in-order dealloc
//   STQ  16 entry  環狀佇列，in-order dealloc（= commit + store buffer drain 代理）
//   MSHR  8 entry  v(1) + countdown(8) + rob(6) + prf(6) + ldq(4) + dead(1)
//   L1-hit 延遲線  8 級 x 2 slot 的純移位暫存器（hit 不佔 MSHR）
//   請求緩衝       8 深 FIFO，每拍最多收 4、送 2（2 個 LSU port）
//
// 建模硬規則：
//   1. 欄位 <= 8 bit，無寬算術（唯一例外是 CONTRACT §1.5 要求的 48-bit counter）
//   2. MAX 尺寸 + cfg_ldq_entries / cfg_stq_entries mask
//   3. 單一 posedge clk + 同步 rst；陣列全是 flat packed reg，索引一律用
//      常數展開的 for 迴圈 → 不會推出 $mem / 非同步讀 / latch
//   4. 沒有 case，全部 if/else 鏈（函式 lat_lut 有 final else）
//   5. 沒有 $display / $finish / initial / DPI
// ===================================================================
module lsu_q (
    input  wire clk, input wire rst, input wire flush,
    input  wire [`LSQ_W:0]            cfg_ldq_entries,
    input  wire [`LSQ_W:0]            cfg_stq_entries,
    input  wire [`W-1:0]         ds_valid,
    input  wire [`W*`RUOP_W-1:0] ds_ruop,
    input  wire [`W*`ROB_W-1:0]  ds_robidx,
    input  wire [`W-1:0]         req_v,
    input  wire [`W*`ROB_W-1:0]  req_rob,
    input  wire [`W*8-1:0]       req_ev,     // 未用，保留
    output wire                  lsu_ready,
    output wire                  lsq_full,
    output wire                  mshr_full,
    output wire [`W-1:0]         done_v,
    output wire [`W*`ROB_W-1:0]  done_rob,
    output wire [`W*`PRF_W-1:0]  done_prf,
    input  wire [`W*8-1:0]       fb_mem_event,
    // v2（監督者裁決）：mem event 是 per fetch-buffer slot，跟 dispatch 差數拍，
    // 所以在本模組內自建 side FIFO：fetch 當拍 push、dispatch 當拍 pop。
    input  wire [`W-1:0]         fb_valid,
    input  wire [`W*`DUOP_W-1:0] fb_duop,
    input  wire [2:0]            fb_take,
    output wire [47:0]           cnt_st_mshr
);
    // ---------------- 可調參數 ----------------
    parameter CONF_THR = 8;    // store-load 衝突機率 = CONF_THR/256（8 -> 3.1%）
    parameter ST_LAT   = 2;    // store 寫進 store buffer 的固定延遲

    // ---- 契約尺寸：一律從 `XXX_N / `XXX_W 推導，不寫死數字（CONTRACT v7 §0）----
    localparam LDN   = `LDQ_N;          // LDQ MAX
    localparam STN   = `STQ_N;          // STQ MAX
    localparam MSN   = `MSHR_N;         // MSHR MAX（cfg_mshr_entries 未接線，見 README §6）
    localparam LQW   = `LSQ_W;          // LDQ/STQ index 寬度
    localparam LQCW  = `LSQ_W + 1;      // LDQ/STQ 佔用計數寬度（要裝得下 MAX 本身）
    localparam MSW   = `MSHR_W;         // MSHR index 寬度
    localparam WCW   = $clog2(`W + 1);  // 0..`W 的計數寬度

    // ---- 本模組自有的結構（不在 CONTRACT 的 MAX 表裡），寬度用 $clog2 自動推導 ----
    localparam HD    = 8;               // L1-hit 延遲線級數
    localparam HDW   = $clog2(HD);
    localparam HS    = HD * 2;          // 每級 2 個 slot
    localparam HSW   = $clog2(HS);
    localparam PQN   = 8;               // LSU 請求緩衝深度
    localparam PQW   = $clog2(PQN);
    localparam PQCW  = $clog2(PQN) + 1;
    localparam MQN   = 32;              // mem-event side FIFO 深度（>= decode queue + rename）
    localparam MQW   = $clog2(MQN);
    localparam MQCW  = $clog2(MQN) + 1;
    localparam MEV_W = 6;               // mem event payload = {lat_class(4), level(2)}
                                        // 跟 PRF_W/ROB_W 無關，不要跟著 sweep 改

    // ---- 有寬度的常數（避免裸數字與寬度不符）----
    localparam [LQCW-1:0]  LD_MAXV  = LDN;
    localparam [LQCW-1:0]  ST_MAXV  = STN;
    localparam [LQCW-1:0]  LQ_ZERO  = {LQCW{1'b0}};
    localparam [LQCW-1:0]  LQ_MINV  = `W;   // 見 §5「最小可表示尺寸」
    localparam [MQCW-1:0]  MQ_MAXV  = MQN;
    localparam [HDW-1:0]   TGT_MAX  = {HDW{1'b1}};  // = HD-1（HD 必須是 2 的冪）
    localparam [7:0]       HD_LAT   = HD;
    localparam [PQCW-1:0]  PQ_ONE   = 1;
    localparam [WCW-1:0]   NL0 = 0, NL1 = 1, NL2 = 2, NL3 = 3;

    // ================= 狀態暫存器 =================
    // ---- LDQ ----
    reg [LDN-1:0]      ld_v, ld_ar, ld_st, ld_blk, ld_dn;
    reg [LDN*`ROB_W-1:0] ld_rob;
    reg [LDN*`PRF_W-1:0] ld_prf;
    reg [LDN*2-1:0]    ld_lvl;      // MEM_LEVEL
    reg [LDN*4-1:0]    ld_lc;       // MEM_LAT_CLASS
    reg [LDN*LQW-1:0]  ld_wid;      // 卡住這條 load 的 STQ index
    reg [LQW-1:0]      ld_head, ld_tail;
    reg [LQCW-1:0]     ld_cnt;      // 0..LDN，要裝得下 MAX 本身

    // ---- STQ ----
    reg [STN-1:0]      st_v, st_ar, st_st, st_dn, st_noacc;
    reg [STN*`ROB_W-1:0] st_rob;
    reg [STN*`PRF_W-1:0] st_prf;
    reg [LQW-1:0]      st_head, st_tail;
    reg [LQCW-1:0]     st_cnt;

    // ---- MSHR ----
    reg [MSN-1:0]      ms_v, ms_dead;
    reg [MSN*8-1:0]    ms_cnt;
    reg [MSN*`ROB_W-1:0] ms_rob;
    reg [MSN*`PRF_W-1:0] ms_prf;
    reg [MSN*LQW-1:0]  ms_ldq;   // LDQ index

    // ---- L1-hit 延遲線（slot s -> stage s>>1）----
    reg [HS-1:0]       hl_v, hl_q;      // hl_q: 1=STQ 0=LDQ
    reg [HS*`ROB_W-1:0]  hl_rob;
    reg [HS*`PRF_W-1:0]  hl_prf;
    reg [HS*LQW-1:0]   hl_idx;   // LDQ/STQ index

    // ---- LSU 請求緩衝（環狀 FIFO）----
    reg [PQN*`ROB_W-1:0] pq_rob;
    reg [PQW-1:0]      pq_head, pq_tail;
    reg [PQCW-1:0]     pq_cnt;

    // ---- mem-event side FIFO：每筆只存 {lat_class[3:0], level[1:0]} = 6 bit ----
    reg [MQN*MEV_W-1:0] me_q;
    reg [MQW-1:0]      me_head, me_tail;   // MQN 是 2 的冪 -> 指標自然折返
    reg [MQCW-1:0]     me_cnt;

    // ---- 決定性偽隨機（store-load disambiguation）----
    reg [7:0]          la, lb;

    reg [47:0]         stmshr;

    // ================= next-state =================
    reg [LDN-1:0]      n_ld_v, n_ld_ar, n_ld_st, n_ld_blk, n_ld_dn;
    reg [LDN*`ROB_W-1:0] n_ld_rob;
    reg [LDN*`PRF_W-1:0] n_ld_prf;
    reg [LDN*2-1:0]    n_ld_lvl;
    reg [LDN*4-1:0]    n_ld_lc;
    reg [LDN*LQW-1:0]  n_ld_wid;
    reg [LQW-1:0]      n_ld_head, n_ld_tail;
    reg [LQCW-1:0]     n_ld_cnt;

    reg [STN-1:0]      n_st_v, n_st_ar, n_st_st, n_st_dn, n_st_noacc;
    reg [STN*`ROB_W-1:0] n_st_rob;
    reg [STN*`PRF_W-1:0] n_st_prf;
    reg [LQW-1:0]      n_st_head, n_st_tail;
    reg [LQCW-1:0]     n_st_cnt;

    reg [MSN-1:0]      n_ms_v, n_ms_dead;
    reg [MSN*8-1:0]    n_ms_cnt;
    reg [MSN*`ROB_W-1:0] n_ms_rob;
    reg [MSN*`PRF_W-1:0] n_ms_prf;
    reg [MSN*LQW-1:0]  n_ms_ldq;

    reg [HS-1:0]       n_hl_v, n_hl_q;
    reg [HS*`ROB_W-1:0]  n_hl_rob;
    reg [HS*`PRF_W-1:0]  n_hl_prf;
    reg [HS*LQW-1:0]   n_hl_idx;

    reg [PQN*`ROB_W-1:0] n_pq_rob;
    reg [PQW-1:0]      n_pq_head, n_pq_tail;
    reg [PQCW-1:0]     n_pq_cnt;

    reg [MQN*MEV_W-1:0] n_me_q;
    reg [MQW-1:0]      n_me_head, n_me_tail;
    reg [MQCW-1:0]     n_me_cnt;

    reg [7:0]          n_la, n_lb;
    reg [47:0]         n_stmshr;

    reg [`W-1:0]       d_v;
    reg [`W*`ROB_W-1:0] d_rob;
    reg [`W*`PRF_W-1:0] d_prf;

    assign done_v      = d_v;
    assign done_rob    = d_rob;
    assign done_prf    = d_prf;
    assign cnt_st_mshr = stmshr;

    // ================= 尺寸 mask（runtime）=================
    // cfg 夾在 [`W, LDQ_N]。下限是 `W：*_ready 是 all-or-nothing（CONTRACT v2），
    // 一組最多 W 條記憶體 uop 必須整組放得下，所以小於 W 的 LSQ 無法表示。
    // 夾住而不是掛掉 —— 這樣 cfg 掃到最小值時模型仍前進（CONTRACT v7 §10.1）。
    wire [LQCW-1:0] ld_cfg  = (cfg_ldq_entries > LD_MAXV) ? LD_MAXV : cfg_ldq_entries;
    wire [LQCW-1:0] st_cfg  = (cfg_stq_entries > ST_MAXV) ? ST_MAXV : cfg_stq_entries;
    wire [LQCW-1:0] ld_max  = (ld_cfg < LQ_MINV) ? LQ_MINV : ld_cfg;
    wire [LQCW-1:0] st_max  = (st_cfg < LQ_MINV) ? LQ_MINV : st_cfg;

    wire [LQCW-1:0] ld_free = ld_max - ld_cnt;
    wire [LQCW-1:0] st_free = st_max - st_cnt;

    // 保守：整組 dispatch（最多 W 條）都放得下才不拉 full
    assign lsq_full  = (ld_free < LQ_MINV) | (st_free < LQ_MINV);
    assign mshr_full = &ms_v;
    // be_eu 會把 pop 決定暫存一拍，所以 ready 只在緩衝全空時拉高，
    // 緩衝深度 PQN=8 足以吸收「ready 慢一拍」造成的 2 個 burst（最大佔用 6）。
    assign lsu_ready = (pq_cnt == {PQCW{1'b0}});

    // ================= 函式 =================
    // bstf_lat_table（include/bstf.h），無負載延遲，cycles
    function [7:0] lat_lut;
        input [3:0] c;
        begin
            if      (c == 4'd0)  lat_lut = 8'd3;
            else if (c == 4'd1)  lat_lut = 8'd4;
            else if (c == 4'd2)  lat_lut = 8'd12;
            else if (c == 4'd3)  lat_lut = 8'd16;
            else if (c == 4'd4)  lat_lut = 8'd30;
            else if (c == 4'd5)  lat_lut = 8'd40;
            else if (c == 4'd6)  lat_lut = 8'd90;
            else if (c == 4'd7)  lat_lut = 8'd120;
            else if (c == 4'd8)  lat_lut = 8'd160;
            else if (c == 4'd9)  lat_lut = 8'd200;
            else if (c == 4'd10) lat_lut = 8'd2;
            else if (c == 4'd11) lat_lut = 8'd8;
            else if (c == 4'd12) lat_lut = 8'd50;
            else if (c == 4'd13) lat_lut = 8'd70;
            else if (c == 4'd14) lat_lut = 8'd140;
            else                 lat_lut = 8'd255;
        end
    endfunction

    // 8-bit Fibonacci LFSR，poly x^8+x^6+x^5+x^4+1（最大週期 255）
    function [7:0] lstep;
        input [7:0] s;
        begin
            lstep = {s[6:0], s[7] ^ s[5] ^ s[4] ^ s[3]};
        end
    endfunction

    // ================= 組合邏輯的暫存 =================
    integer e, m, p, k, s, i, j;

    reg [LQW-1:0]  dh;
    reg        hv_, hd_;
    reg [1:0]  ndl_ld, ndl_st;
    reg        sdv0, sdv1;
    reg [LQW-1:0]  sdi0, sdi1;
    reg [1:0]  nrep;
    reg        rv0, rv1;
    reg [LQW-1:0]  ri0, ri1;
    reg        sv0, sv1;
    reg [`ROB_W-1:0] sr0, sr1;
    reg [PQW-1:0]  ph, pt;
    reg [1:0]  nsv;
    reg [WCW-1:0]  nacc;
    reg [PQCW-1:0] pcnt;

    reg [1:0]  ngr;
    reg [1:0]  g_val, g_isld;
    reg [LQW-1:0]  g_idx0, g_idx1;
    reg [3:0]  g_lc0,  g_lc1;
    reg [1:0]  g_lvl0, g_lvl1;
    reg [`ROB_W-1:0] g_rob0, g_rob1;
    reg [`PRF_W-1:0] g_prf0, g_prf1;

    reg [LQW-1:0]  c_idx;
    reg [3:0]  c_lc;
    reg [1:0]  c_lvl;
    reg [`ROB_W-1:0] c_rob;
    reg [`PRF_W-1:0] c_prf;
    reg        c_isld;
    reg [7:0]  c_lat;
    reg [HDW-1:0]  c_tgt;
    reg        placed, got_ms;
    reg [MSW-1:0]  ms_sel;
    reg        mshr_stall;

    reg [7:0]  la1, la2, la3, la4;
    reg [7:0]  lb0, lb1, lb2, lb3, lb4;
    reg [7:0]  rnd;
    reg [WCW-1:0]  nld_lane;
    reg [LQW-1:0]  a_ldt, a_stt;
    reg [LQCW-1:0] a_ldc, a_stc;
    // verilator lint_off UNUSEDSIGNAL
    reg [`RUOP_W-1:0] u;
    reg [3:0]  ucls;
    reg [7:0]  uev;
    // verilator lint_on UNUSEDSIGNAL
    reg        is_ld, is_st, is_amo, is_mem;
    reg [MQW-1:0]  mh, mt;
    reg [WCW-1:0]  nmpop, nmpush;
    reg [MEV_W-1:0] evq;
    // verilator lint_off UNUSEDSIGNAL
    reg [`DUOP_W-1:0] du;
    reg [3:0]  dcls;
    // verilator lint_on UNUSEDSIGNAL
    reg        ftk;
    reg        conflict;
    reg [LQW-1:0]  wid_i;
    reg [LQCW-1:0] tmp5;   // 環狀指標 +1 的中間值

    // verilator lint_off UNUSED
    wire [`W*8-1:0] unused_req_ev = req_ev;
    // verilator lint_on UNUSED

    always @* begin
        // ---------- 0. 預設：維持現狀 ----------
        n_ld_v = ld_v; n_ld_ar = ld_ar; n_ld_st = ld_st;
        n_ld_blk = ld_blk; n_ld_dn = ld_dn;
        n_ld_rob = ld_rob; n_ld_prf = ld_prf;
        n_ld_lvl = ld_lvl; n_ld_lc = ld_lc; n_ld_wid = ld_wid;
        n_ld_head = ld_head; n_ld_tail = ld_tail; n_ld_cnt = ld_cnt;

        n_st_v = st_v; n_st_ar = st_ar; n_st_st = st_st;
        n_st_dn = st_dn; n_st_noacc = st_noacc;
        n_st_rob = st_rob; n_st_prf = st_prf;
        n_st_head = st_head; n_st_tail = st_tail; n_st_cnt = st_cnt;

        n_ms_v = ms_v; n_ms_dead = ms_dead; n_ms_cnt = ms_cnt;
        n_ms_rob = ms_rob; n_ms_prf = ms_prf; n_ms_ldq = ms_ldq;

        n_hl_v = {HS{1'b0}}; n_hl_q = {HS{1'b0}};
        n_hl_rob = {HS*`ROB_W{1'b0}}; n_hl_prf = {HS*`PRF_W{1'b0}};
        n_hl_idx = {HS*LQW{1'b0}};

        n_pq_rob = pq_rob; n_pq_head = pq_head;
        n_pq_tail = pq_tail; n_pq_cnt = pq_cnt;
        n_me_q = me_q; n_me_head = me_head;
        n_me_tail = me_tail; n_me_cnt = me_cnt;
        n_stmshr = stmshr;

        d_v = {`W{1'b0}}; d_rob = {`W*`ROB_W{1'b0}}; d_prf = {`W*`PRF_W{1'b0}};

        mshr_stall = 1'b0;
        ndl_ld = 2'd0; ndl_st = 2'd0;
        sdv0 = 1'b0; sdv1 = 1'b0; sdi0 = {LQW{1'b0}}; sdi1 = {LQW{1'b0}};
        nrep = 2'd0; ngr = 2'd0;
        rv0 = 1'b0; rv1 = 1'b0; ri0 = {LQW{1'b0}}; ri1 = {LQW{1'b0}};
        sv0 = 1'b0; sv1 = 1'b0;
        sr0 = {`ROB_W{1'b0}}; sr1 = {`ROB_W{1'b0}};
        ph = pq_head; pt = pq_tail; nsv = 2'd0;
        nacc = {WCW{1'b0}}; pcnt = {PQCW{1'b0}};
        g_val = 2'b00; g_isld = 2'b00;
        g_idx0 = {LQW{1'b0}}; g_idx1 = {LQW{1'b0}}; g_lc0 = 4'd0; g_lc1 = 4'd0;
        g_lvl0 = 2'd0; g_lvl1 = 2'd0;
        g_rob0 = {`ROB_W{1'b0}}; g_rob1 = {`ROB_W{1'b0}};
        g_prf0 = {`PRF_W{1'b0}}; g_prf1 = {`PRF_W{1'b0}};
        c_idx = {LQW{1'b0}}; c_lc = 4'd0; c_lvl = 2'd0;
        c_rob = {`ROB_W{1'b0}}; c_prf = {`PRF_W{1'b0}};
        c_isld = 1'b0; c_lat = 8'd0; c_tgt = {HDW{1'b0}};
        placed = 1'b0; got_ms = 1'b0; ms_sel = {MSW{1'b0}};
        dh = {LQW{1'b0}}; hv_ = 1'b0; hd_ = 1'b0;
        rnd = 8'd0; nld_lane = {WCW{1'b0}}; conflict = 1'b0; wid_i = {LQW{1'b0}};
        u = {`RUOP_W{1'b0}}; ucls = 4'd0; uev = 8'd0;
        is_ld = 1'b0; is_st = 1'b0; is_amo = 1'b0; is_mem = 1'b0; tmp5 = LQ_ZERO;
        mh = me_head; mt = me_tail;
        nmpop = {WCW{1'b0}}; nmpush = {WCW{1'b0}};
        evq = {MEV_W{1'b0}}; du = {`DUOP_W{1'b0}}; dcls = 4'd0; ftk = 1'b0;
        a_ldt = {LQW{1'b0}}; a_stt = {LQW{1'b0}};
        a_ldc = LQ_ZERO;     a_stc = LQ_ZERO;
        la1 = 8'd0; la2 = 8'd0; la3 = 8'd0; la4 = 8'd0;
        lb0 = 8'd0; lb1 = 8'd0; lb2 = 8'd0; lb3 = 8'd0; lb4 = 8'd0;

        // ---------- 1. LDQ dealloc（head，最多 2/cycle）----------
        dh = ld_head;
        for (k = 0; k < 2; k = k + 1) begin
            hv_ = 1'b0; hd_ = 1'b0;
            for (e = 0; e < LDN; e = e + 1)
                if (e[LQW-1:0] == dh) begin hv_ = n_ld_v[e]; hd_ = n_ld_dn[e]; end
            if (hv_ & hd_) begin
                for (e = 0; e < LDN; e = e + 1)
                    if (e[LQW-1:0] == dh) begin
                        n_ld_v[e]   = 1'b0; n_ld_ar[e]  = 1'b0;
                        n_ld_st[e]  = 1'b0; n_ld_dn[e]  = 1'b0;
                        n_ld_blk[e] = 1'b0;
                    end
                ndl_ld = ndl_ld + 2'd1;
                tmp5 = {{(LQCW-LQW){1'b0}}, dh} + 1'b1;
                if (tmp5 >= ld_max) dh = {LQW{1'b0}};
                else                dh = tmp5[LQW-1:0];
            end
        end
        n_ld_head = dh;
        n_ld_cnt  = ld_cnt - {{(LQCW-2){1'b0}}, ndl_ld};

        // ---------- 2. STQ dealloc（store commit / store buffer drain 代理）----
        dh = st_head;
        for (k = 0; k < 2; k = k + 1) begin
            hv_ = 1'b0; hd_ = 1'b0;
            for (e = 0; e < STN; e = e + 1)
                if (e[LQW-1:0] == dh) begin hv_ = n_st_v[e]; hd_ = n_st_dn[e]; end
            if (hv_ & hd_) begin
                for (e = 0; e < STN; e = e + 1)
                    if (e[LQW-1:0] == dh) begin
                        n_st_v[e]     = 1'b0; n_st_ar[e] = 1'b0;
                        n_st_st[e]    = 1'b0; n_st_dn[e] = 1'b0;
                        n_st_noacc[e] = 1'b0;
                    end
                if (k == 0) begin sdv0 = 1'b1; sdi0 = dh; end
                else        begin sdv1 = 1'b1; sdi1 = dh; end
                ndl_st = ndl_st + 2'd1;
                tmp5 = {{(LQCW-LQW){1'b0}}, dh} + 1'b1;
                if (tmp5 >= st_max) dh = {LQW{1'b0}};
                else                dh = tmp5[LQW-1:0];
            end
        end
        n_st_head = dh;
        n_st_cnt  = st_cnt - {{(LQCW-2){1'b0}}, ndl_st};

        // ---------- 3. 解除 store-load 衝突封鎖 ----------
        for (e = 0; e < LDN; e = e + 1)
            if (n_ld_blk[e] &
                ((sdv0 & (n_ld_wid[e*LQW +: LQW] == sdi0)) |
                 (sdv1 & (n_ld_wid[e*LQW +: LQW] == sdi1))))
                n_ld_blk[e] = 1'b0;

        // ---------- 4. 延遲線 stage0 完成 → done lane 0/1 ----------
        for (s = 0; s < 2; s = s + 1)
            if (hl_v[s]) begin
                d_v[s] = 1'b1;
                d_rob[s*`ROB_W +: `ROB_W] = hl_rob[s*`ROB_W +: `ROB_W];
                d_prf[s*`PRF_W +: `PRF_W] = hl_prf[s*`PRF_W +: `PRF_W];
                if (hl_q[s]) begin
                    for (e = 0; e < STN; e = e + 1)
                        if (e[LQW-1:0] == hl_idx[s*LQW +: LQW]) n_st_dn[e] = 1'b1;
                end else begin
                    for (e = 0; e < LDN; e = e + 1)
                        if (e[LQW-1:0] == hl_idx[s*LQW +: LQW]) n_ld_dn[e] = 1'b1;
                end
            end

        // ---------- 5. 延遲線移位（stage k <= stage k+1）----------
        for (s = 0; s < HS - 2; s = s + 1) begin
            n_hl_v[s]          = hl_v[s+2];
            n_hl_q[s]          = hl_q[s+2];
            n_hl_rob[s*`ROB_W +: `ROB_W] = hl_rob[(s+2)*`ROB_W +: `ROB_W];
            n_hl_prf[s*`PRF_W +: `PRF_W] = hl_prf[(s+2)*`PRF_W +: `PRF_W];
            n_hl_idx[s*LQW +: LQW] = hl_idx[(s+2)*LQW +: LQW];
        end

        // ---------- 6. MSHR 倒數 + 完成回報（done lane 2/3，最多 2/cycle）----
        for (m = 0; m < MSN; m = m + 1) begin
            if (ms_v[m]) begin
                if (ms_cnt[m*8 +: 8] != 8'd0) begin
                    n_ms_cnt[m*8 +: 8] = ms_cnt[m*8 +: 8] - 8'd1;
                end else if (ms_dead[m]) begin
                    // flush 之後的 wrong-path miss：照樣佔著 MSHR 倒數完，
                    // 但不回報（pipeline 其他級在 flush 時已經整組清空）
                    n_ms_v[m]    = 1'b0;
                    n_ms_dead[m] = 1'b0;
                end else if (nrep < 2'd2) begin
                    if (nrep == 2'd0) begin
                        d_v[2] = 1'b1;
                        d_rob[2*`ROB_W +: `ROB_W] = ms_rob[m*`ROB_W +: `ROB_W];
                        d_prf[2*`PRF_W +: `PRF_W] = ms_prf[m*`PRF_W +: `PRF_W];
                        rv0 = 1'b1; ri0 = ms_ldq[m*LQW +: LQW];
                    end else begin
                        d_v[3] = 1'b1;
                        d_rob[3*`ROB_W +: `ROB_W] = ms_rob[m*`ROB_W +: `ROB_W];
                        d_prf[3*`PRF_W +: `PRF_W] = ms_prf[m*`PRF_W +: `PRF_W];
                        rv1 = 1'b1; ri1 = ms_ldq[m*LQW +: LQW];
                    end
                    n_ms_v[m] = 1'b0;
                    nrep = nrep + 2'd1;
                end
            end
        end
        // 只對真正回報的（最多 2 個）做一次 LDQ index 解碼
        for (e = 0; e < LDN; e = e + 1)
            if ((rv0 & (e[LQW-1:0] == ri0)) | (rv1 & (e[LQW-1:0] == ri1)))
                n_ld_dn[e] = 1'b1;

        // ---------- 7. 請求緩衝：送 2 個進 rob CAM，設 address-ready ----------
        if (pq_cnt > {PQCW{1'b0}}) begin
            sv0 = 1'b1;
            for (j = 0; j < PQN; j = j + 1)
                if (j[PQW-1:0] == ph) sr0 = pq_rob[j*`ROB_W +: `ROB_W];
            ph = ph + 1'b1; nsv = 2'd1;
        end
        if (pq_cnt > PQ_ONE) begin
            sv1 = 1'b1;
            for (j = 0; j < PQN; j = j + 1)
                if (j[PQW-1:0] == ph) sr1 = pq_rob[j*`ROB_W +: `ROB_W];
            ph = ph + 1'b1; nsv = 2'd2;
        end
        n_pq_head = ph;

        for (e = 0; e < LDN; e = e + 1)
            if (n_ld_v[e] & ~n_ld_ar[e] &
                ((sv0 & (n_ld_rob[e*`ROB_W +: `ROB_W] == sr0)) |
                 (sv1 & (n_ld_rob[e*`ROB_W +: `ROB_W] == sr1))))
                n_ld_ar[e] = 1'b1;
        for (e = 0; e < STN; e = e + 1)
            if (n_st_v[e] & ~n_st_ar[e] &
                ((sv0 & (n_st_rob[e*`ROB_W +: `ROB_W] == sr0)) |
                 (sv1 & (n_st_rob[e*`ROB_W +: `ROB_W] == sr1))))
                n_st_ar[e] = 1'b1;

        // ---------- 8a. 記憶體埠仲裁：最多 2 個存取 / cycle（load 優先）------
        for (e = 0; e < LDN; e = e + 1)
            if ((ngr < 2'd2) & n_ld_v[e] & n_ld_ar[e] &
                ~n_ld_st[e] & ~n_ld_blk[e] & ~n_ld_dn[e]) begin
                if (ngr == 2'd0) begin
                    g_val[0] = 1'b1; g_isld[0] = 1'b1; g_idx0 = e[LQW-1:0];
                    g_lc0  = n_ld_lc[e*4 +: 4];  g_lvl0 = n_ld_lvl[e*2 +: 2];
                    g_rob0 = n_ld_rob[e*`ROB_W +: `ROB_W]; g_prf0 = n_ld_prf[e*`PRF_W +: `PRF_W];
                end else begin
                    g_val[1] = 1'b1; g_isld[1] = 1'b1; g_idx1 = e[LQW-1:0];
                    g_lc1  = n_ld_lc[e*4 +: 4];  g_lvl1 = n_ld_lvl[e*2 +: 2];
                    g_rob1 = n_ld_rob[e*`ROB_W +: `ROB_W]; g_prf1 = n_ld_prf[e*`PRF_W +: `PRF_W];
                end
                ngr = ngr + 2'd1;
            end
        for (e = 0; e < STN; e = e + 1)
            if ((ngr < 2'd2) & n_st_v[e] & n_st_ar[e] &
                ~n_st_st[e] & ~n_st_dn[e] & ~n_st_noacc[e]) begin
                if (ngr == 2'd0) begin
                    g_val[0] = 1'b1; g_isld[0] = 1'b0; g_idx0 = e[LQW-1:0];
                    g_lc0 = 4'd10; g_lvl0 = 2'd0;
                    g_rob0 = n_st_rob[e*`ROB_W +: `ROB_W]; g_prf0 = n_st_prf[e*`PRF_W +: `PRF_W];
                end else begin
                    g_val[1] = 1'b1; g_isld[1] = 1'b0; g_idx1 = e[LQW-1:0];
                    g_lc1 = 4'd10; g_lvl1 = 2'd0;
                    g_rob1 = n_st_rob[e*`ROB_W +: `ROB_W]; g_prf1 = n_st_prf[e*`PRF_W +: `PRF_W];
                end
                ngr = ngr + 2'd1;
            end

        // ---------- 8b. 發動存取：L1 hit 走延遲線，miss 一定要配 MSHR ----------
        for (p = 0; p < 2; p = p + 1) begin
            if (g_val[p]) begin
                if (p == 0) begin
                    c_idx = g_idx0; c_lc = g_lc0; c_lvl = g_lvl0;
                    c_rob = g_rob0; c_prf = g_prf0; c_isld = g_isld[0];
                end else begin
                    c_idx = g_idx1; c_lc = g_lc1; c_lvl = g_lvl1;
                    c_rob = g_rob1; c_prf = g_prf1; c_isld = g_isld[1];
                end
                if (c_isld) c_lat = lat_lut(c_lc);
                else        c_lat = ST_LAT[7:0];

                if (c_isld & (c_lvl != 2'd0)) begin
                    // ---- miss ----
                    got_ms = 1'b0; ms_sel = {MSW{1'b0}};
                    for (m = 0; m < MSN; m = m + 1)
                        if (~got_ms & ~n_ms_v[m]) begin
                            got_ms = 1'b1; ms_sel = m[MSW-1:0];
                        end
                    if (got_ms) begin
                        for (m = 0; m < MSN; m = m + 1)
                            if (m[MSW-1:0] == ms_sel) begin
                                n_ms_v[m]          = 1'b1;
                                n_ms_dead[m]       = 1'b0;
                                n_ms_cnt[m*8 +: 8] = c_lat;
                                n_ms_rob[m*`ROB_W +: `ROB_W] = c_rob;
                                n_ms_prf[m*`PRF_W +: `PRF_W] = c_prf;
                                n_ms_ldq[m*LQW +: LQW] = c_idx;
                            end
                        for (e = 0; e < LDN; e = e + 1)
                            if (e[LQW-1:0] == c_idx) n_ld_st[e] = 1'b1;
                    end else begin
                        // MSHR 滿 → 這個 miss 卡住，下一拍重試（MLP 上限）
                        mshr_stall = 1'b1;
                    end
                end else begin
                    // ---- L1 hit / store：固定延遲，走延遲線 ----
                    if (c_lat == 8'd0)     c_tgt = {HDW{1'b0}};
                    else if (c_lat > HD_LAT) c_tgt = TGT_MAX;
                    else                   c_tgt = c_lat[HDW-1:0] - 1'b1;
                    placed = 1'b0;
                    for (s = 0; s < HS; s = s + 1)
                        if (~placed & (s[HSW-1:1] == c_tgt) & ~n_hl_v[s]) begin
                            n_hl_v[s]          = 1'b1;
                            n_hl_q[s]          = ~c_isld;
                            n_hl_rob[s*`ROB_W +: `ROB_W] = c_rob;
                            n_hl_prf[s*`PRF_W +: `PRF_W] = c_prf;
                            n_hl_idx[s*LQW +: LQW] = c_idx;
                            placed = 1'b1;
                        end
                    if (placed) begin
                        if (c_isld) begin
                            for (e = 0; e < LDN; e = e + 1)
                                if (e[LQW-1:0] == c_idx) n_ld_st[e] = 1'b1;
                        end else begin
                            for (e = 0; e < STN; e = e + 1)
                                if (e[LQW-1:0] == c_idx) n_st_st[e] = 1'b1;
                        end
                    end
                end
            end
        end

        // ---------- 9. 收新的 LSU 請求（每拍最多 W 個）----------
        pcnt = pq_cnt - {{(PQCW-2){1'b0}}, nsv};
        for (i = 0; i < `W; i = i + 1)
            if (req_v[i] & ((pcnt + {{(PQCW-WCW){1'b0}}, nacc}) < PQN[3:0])) begin
                for (j = 0; j < PQN; j = j + 1)
                    if (j[PQW-1:0] == pt)
                        n_pq_rob[j*`ROB_W +: `ROB_W] = req_rob[i*`ROB_W +: `ROB_W];
                pt   = pt + 1'b1;
                nacc = nacc + 1'b1;
            end
        n_pq_tail = pt;
        n_pq_cnt  = pcnt + {{(PQCW-WCW){1'b0}}, nacc};

        // ---------- 10. dispatch 配置 LDQ / STQ ----------
        la1 = lstep(la);  la2 = lstep(la1); la3 = lstep(la2); la4 = lstep(la3);
        lb0 = lb;         lb1 = lstep(lb0); lb2 = lstep(lb1);
        lb3 = lstep(lb2); lb4 = lstep(lb3);

        a_ldt = n_ld_tail; a_stt = n_st_tail;
        a_ldc = n_ld_cnt;  a_stc = n_st_cnt;

        for (i = 0; i < `W; i = i + 1) begin
            u    = ds_ruop[i*`RUOP_W +: `RUOP_W];
            ucls = u[`RUOP_CLASS];
            is_amo = ds_valid[i] & ~flush & (ucls == `UC_AMO);
            is_ld  = (ds_valid[i] & ~flush & (ucls == `UC_LOAD))  | is_amo;
            is_st  = (ds_valid[i] & ~flush & (ucls == `UC_STORE)) | is_amo;
            is_mem = is_ld | is_st;          // AMO 只算一筆 mem access

            // ---- 從 side FIFO 取出這條 mem uop 的 cache 事件 ----
            evq = {MEV_W{1'b0}};
            if (is_mem & ({{(MQCW-WCW){1'b0}}, nmpop} < me_cnt)) begin
                for (j = 0; j < MQN; j = j + 1)
                    if (j[MQW-1:0] == mh) evq = me_q[j*MEV_W +: MEV_W];
                mh    = mh + 1'b1;
                nmpop = nmpop + 1'b1;
            end
            uev = {evq[5:2], 2'b00, evq[1:0]};   // {lat_class, -, level}

            // ---- STQ 先配（同一拍的 store 不算自己的 hazard）----
            if (is_st & (a_stc < st_max)) begin
                for (e = 0; e < STN; e = e + 1)
                    if (e[LQW-1:0] == a_stt) begin
                        n_st_v[e]  = 1'b1;
                        n_st_ar[e] = 1'b0;
                        n_st_st[e] = 1'b0;
                        n_st_rob[e*`ROB_W +: `ROB_W] = ds_robidx[i*`ROB_W +: `ROB_W];
                        n_st_prf[e*`PRF_W +: `PRF_W] = u[`RUOP_D];
                        // AMO 的 STQ entry 只是排序佔位：不佔埠、不回報 done
                        n_st_noacc[e] = is_amo;
                        n_st_dn[e]    = is_amo;
                    end
                tmp5 = {{(LQCW-LQW){1'b0}}, a_stt} + 1'b1;
                if (tmp5 >= st_max) a_stt = {LQW{1'b0}};
                else                a_stt = tmp5[LQW-1:0];
                a_stc = a_stc + 1'b1;
            end

            // ---- LDQ ----
            if (is_ld & (a_ldc < ld_max)) begin
                // 決定性偽隨機：rnd < CONF_THR 且有更早未完成的 store → 衝突
                if      (i == 0) rnd = la1 ^ lb0;
                else if (i == 1) rnd = la2 ^ lb0;
                else if (i == 2) rnd = la3 ^ lb0;
                else             rnd = la4 ^ lb0;
                // STQ 裡最年輕的 store（= a_stt - 1）一定比這條 load 老
                if (a_stt == {LQW{1'b0}}) wid_i = st_max[LQW-1:0] - 1'b1;
                else                      wid_i = a_stt - 1'b1;
                conflict = (rnd < CONF_THR) & (a_stc != LQ_ZERO);

                for (e = 0; e < LDN; e = e + 1)
                    if (e[LQW-1:0] == a_ldt) begin
                        n_ld_v[e]   = 1'b1;
                        n_ld_ar[e]  = 1'b0;
                        n_ld_st[e]  = 1'b0;
                        n_ld_dn[e]  = 1'b0;
                        n_ld_blk[e] = conflict;
                        n_ld_wid[e*LQW +: LQW] = wid_i;
                        n_ld_rob[e*`ROB_W +: `ROB_W] = ds_robidx[i*`ROB_W +: `ROB_W];
                        n_ld_prf[e*`PRF_W +: `PRF_W] = u[`RUOP_D];
                        n_ld_lvl[e*2 +: 2] = uev[1:0];      // MEM_LEVEL
                        n_ld_lc[e*4 +: 4]  = uev[7:4];      // MEM_LAT_CLASS
                    end
                tmp5 = {{(LQCW-LQW){1'b0}}, a_ldt} + 1'b1;
                if (tmp5 >= ld_max) a_ldt = {LQW{1'b0}};
                else                a_ldt = tmp5[LQW-1:0];
                a_ldc = a_ldc + 1'b1;
                nld_lane = nld_lane + 1'b1;
            end
        end
        n_ld_tail = a_ldt; n_ld_cnt = a_ldc;
        n_st_tail = a_stt; n_st_cnt = a_stc;
        n_me_head = mh;

        // ---- mem-event side FIFO：fetch 當拍實際被取走的 mem uop 推進來 ----
        for (i = 0; i < `W; i = i + 1) begin
            du   = fb_duop[i*`DUOP_W +: `DUOP_W];
            dcls = du[`DUOP_CLASS];
            ftk  = fb_valid[i] & (i[WCW-1:0] < fb_take);
            if (ftk & ((dcls == `UC_LOAD) | (dcls == `UC_STORE) | (dcls == `UC_AMO))
                    & ((me_cnt - {{(MQCW-WCW){1'b0}}, nmpop} + {{(MQCW-WCW){1'b0}}, nmpush}) < MQ_MAXV)) begin
                for (j = 0; j < MQN; j = j + 1)
                    if (j[MQW-1:0] == mt)
                        n_me_q[j*MEV_W +: MEV_W] = {fb_mem_event[i*8+4 +: 4],
                                            fb_mem_event[i*8 +: 2]};
                mt     = mt + 1'b1;
                nmpush = nmpush + 1'b1;
            end
        end
        n_me_tail = mt;
        n_me_cnt  = me_cnt - {{(MQCW-WCW){1'b0}}, nmpop} + {{(MQCW-WCW){1'b0}}, nmpush};

        // LFSR：la 每拍 4 步，lb 每配一條 load 1 步（兩者相位錯開 → 長週期）
        n_la = la4;
        if      (nld_lane == NL0) n_lb = lb0;
        else if (nld_lane == NL1) n_lb = lb1;
        else if (nld_lane == NL2) n_lb = lb2;
        else if (nld_lane == NL3) n_lb = lb3;
        else                       n_lb = lb4;

        // ---------- 11. flush：LSQ / 延遲線 / 請求緩衝整組清空 ----------
        // （be_eu、be_dispatch 在 flush 時也是整組清空，語意一致）
        // MSHR 例外：已經送出去的 miss 收不回來，改標 dead 繼續倒數，
        // 佔著 MSHR 但不回報 —— 這樣才保得住 wrong-path 的 MSHR 污染效應。
        if (flush) begin
            n_ld_v = {LDN{1'b0}}; n_ld_ar = {LDN{1'b0}}; n_ld_st = {LDN{1'b0}};
            n_ld_blk = {LDN{1'b0}}; n_ld_dn = {LDN{1'b0}};
            n_ld_head = {LQW{1'b0}}; n_ld_tail = {LQW{1'b0}}; n_ld_cnt = LQ_ZERO;

            n_st_v = {STN{1'b0}}; n_st_ar = {STN{1'b0}}; n_st_st = {STN{1'b0}};
            n_st_dn = {STN{1'b0}}; n_st_noacc = {STN{1'b0}};
            n_st_head = {LQW{1'b0}}; n_st_tail = {LQW{1'b0}}; n_st_cnt = LQ_ZERO;

            n_hl_v = {HS{1'b0}};
            n_pq_head = {PQW{1'b0}}; n_pq_tail = {PQW{1'b0}}; n_pq_cnt = {PQCW{1'b0}};
            // wrong-path 的 mem event 一律丟掉（runtime 會把 .mem 游標倒回）
            n_me_head = {MQW{1'b0}}; n_me_tail = {MQW{1'b0}}; n_me_cnt = {MQCW{1'b0}};
            n_ms_dead = n_ms_dead | n_ms_v;
            d_v = {`W{1'b0}};
        end

        // ---------- 12. counter ----------
        if (mshr_stall) n_stmshr = stmshr + 48'd1;
    end

    // ================= 同步更新 =================
    always @(posedge clk) begin
        if (rst) begin
            ld_v <= {LDN{1'b0}}; ld_ar <= {LDN{1'b0}}; ld_st <= {LDN{1'b0}};
            ld_blk <= {LDN{1'b0}}; ld_dn <= {LDN{1'b0}};
            ld_rob <= {LDN*`ROB_W{1'b0}}; ld_prf <= {LDN*`PRF_W{1'b0}};
            ld_lvl <= {LDN*2{1'b0}}; ld_lc <= {LDN*4{1'b0}};
            ld_wid <= {LDN*LQW{1'b0}};
            ld_head <= {LQW{1'b0}}; ld_tail <= {LQW{1'b0}}; ld_cnt <= LQ_ZERO;

            st_v <= {STN{1'b0}}; st_ar <= {STN{1'b0}}; st_st <= {STN{1'b0}};
            st_dn <= {STN{1'b0}}; st_noacc <= {STN{1'b0}};
            st_rob <= {STN*`ROB_W{1'b0}}; st_prf <= {STN*`PRF_W{1'b0}};
            st_head <= {LQW{1'b0}}; st_tail <= {LQW{1'b0}}; st_cnt <= LQ_ZERO;

            ms_v <= {MSN{1'b0}}; ms_dead <= {MSN{1'b0}};
            ms_cnt <= {MSN*8{1'b0}};
            ms_rob <= {MSN*`ROB_W{1'b0}}; ms_prf <= {MSN*`PRF_W{1'b0}};
            ms_ldq <= {MSN*LQW{1'b0}};

            hl_v <= {HS{1'b0}}; hl_q <= {HS{1'b0}};
            hl_rob <= {HS*`ROB_W{1'b0}}; hl_prf <= {HS*`PRF_W{1'b0}};
            hl_idx <= {HS*LQW{1'b0}};

            pq_rob <= {PQN*`ROB_W{1'b0}};
            pq_head <= {PQW{1'b0}}; pq_tail <= {PQW{1'b0}}; pq_cnt <= {PQCW{1'b0}};

            me_q <= {MQN*MEV_W{1'b0}};
            me_head <= {MQW{1'b0}}; me_tail <= {MQW{1'b0}}; me_cnt <= {MQCW{1'b0}};

            la <= 8'hA5; lb <= 8'h3C;
            stmshr <= 48'd0;
        end else begin
            ld_v <= n_ld_v; ld_ar <= n_ld_ar; ld_st <= n_ld_st;
            ld_blk <= n_ld_blk; ld_dn <= n_ld_dn;
            ld_rob <= n_ld_rob; ld_prf <= n_ld_prf;
            ld_lvl <= n_ld_lvl; ld_lc <= n_ld_lc; ld_wid <= n_ld_wid;
            ld_head <= n_ld_head; ld_tail <= n_ld_tail; ld_cnt <= n_ld_cnt;

            st_v <= n_st_v; st_ar <= n_st_ar; st_st <= n_st_st;
            st_dn <= n_st_dn; st_noacc <= n_st_noacc;
            st_rob <= n_st_rob; st_prf <= n_st_prf;
            st_head <= n_st_head; st_tail <= n_st_tail; st_cnt <= n_st_cnt;

            ms_v <= n_ms_v; ms_dead <= n_ms_dead; ms_cnt <= n_ms_cnt;
            ms_rob <= n_ms_rob; ms_prf <= n_ms_prf; ms_ldq <= n_ms_ldq;

            hl_v <= n_hl_v; hl_q <= n_hl_q;
            hl_rob <= n_hl_rob; hl_prf <= n_hl_prf; hl_idx <= n_hl_idx;

            pq_rob <= n_pq_rob;
            pq_head <= n_pq_head; pq_tail <= n_pq_tail; pq_cnt <= n_pq_cnt;

            me_q <= n_me_q;
            me_head <= n_me_head; me_tail <= n_me_tail; me_cnt <= n_me_cnt;

            la <= n_la; lb <= n_lb;
            stmshr <= n_stmshr;
        end
    end
endmodule
