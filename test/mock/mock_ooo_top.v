// ===================================================================
// mock_ooo_top — Agent G 的假模型
//
// 目的：在真 OOO 模型（Agent D/E/F）到位之前，讓 runtime 可以端到端
//       跑起來、也讓 test/equiv 的對拍框架有第二個目標。
// 埠列**完全照** model/ooo/top.v，行為刻意極簡：
//   * 每拍吃 min(有效筆數, cfg_fetch_width) 筆，ROB 滿就吃 0
//   * 每拍 retire min(occ, cfg_commit_width) 筆（預設 cfg_commit_width=2）
//   * fe_event 說 REDIRECT 且方向預測錯 -> 跳 shadow，12 拍後跳回
// 遵守 CONTRACT §1：欄位 <= 8 bit、同步單 clock、case 有 default、
// 無 $display/$finish/DPI。（counter 本身 48 bit，與 top.v 的 cnt_* 相同。）
// ===================================================================
`include "common/ifc.vh"

module mock_ooo_top (
    input  wire                     clk,
    input  wire                     rst,

    input  wire [6:0]               cfg_rob_entries,
    input  wire [5:0]               cfg_iq_entries,
    input  wire [4:0]               cfg_ldq_entries,
    input  wire [4:0]               cfg_stq_entries,
    input  wire [3:0]               cfg_mshr_entries,
    input  wire [2:0]               cfg_fetch_width,
    input  wire [2:0]               cfg_issue_width,
    input  wire [2:0]               cfg_commit_width,

    input  wire [`W-1:0]            fb_valid,
    input  wire [`W*`DUOP_W-1:0]    fb_duop,
    input  wire [7:0]               fb_fe_event,
    input  wire [`W*8-1:0]          fb_mem_event,
    output wire [2:0]               fb_take,
    output wire                     fb_redirect,
    output wire                     fb_redir_shadow,

    output wire [47:0]              cnt_cycles,
    output wire [47:0]              cnt_retired,
    output wire [47:0]              cnt_wrongpath,
    output wire [47:0]              cnt_st_fetch,
    output wire [47:0]              cnt_st_refill,
    output wire [47:0]              cnt_st_rename,
    output wire [47:0]              cnt_st_iq,
    output wire [47:0]              cnt_st_rob,
    output wire [47:0]              cnt_st_lsq,
    output wire [47:0]              cnt_st_mshr,
    output wire [47:0]              cnt_mispred,
    output wire [47:0]              cnt_rob_occ_sum
);
    reg  [6:0]  occ;        // 全部 in-flight（含 wrong-path）
    reg  [6:0]  occ_cp;     // 其中 correct-path 的部分，只有這些會 retire
    reg         in_shadow;
    reg  [5:0]  shadow_cnt;

    reg  [47:0] c_cycles, c_retired, c_wrongpath, c_st_fetch, c_st_refill, c_st_rename;
    reg  [47:0] c_st_iq, c_st_rob, c_st_lsq, c_st_mshr, c_mispred, c_rob_occ;

    // ---- 前端 ----
    wire [2:0] nvalid = {2'b0, fb_valid[0]} + {2'b0, fb_valid[1]}
                      + {2'b0, fb_valid[2]} + {2'b0, fb_valid[3]};
    wire       fw_lt  = (nvalid < cfg_fetch_width);
    wire [2:0] want   = fw_lt ? nvalid : cfg_fetch_width;

    // ---- ROB ----
    wire [7:0] sum      = {1'b0, occ} + {5'b0, want};
    wire       rob_full = (sum > {1'b0, cfg_rob_entries});
    wire [2:0] take     = rob_full ? 3'd0 : want;
    // retire 只看 correct-path 的佔用 -> wrong-path uop 永遠不會被 retire
    wire       cw_gt    = (occ_cp < {4'b0, cfg_commit_width});
    wire [2:0] ret      = cw_gt ? occ_cp[2:0] : cfg_commit_width;

    // 每個 slot 的 wrongpath 位元（runtime 在 shadow 區的記錄上會設它）。
    // 逐 slot 數，而不是只看第 0 筆 —— window 可能正好跨在 cp/wp 邊界上。
    wire [3:0] wpb = { fb_duop[3*`DUOP_W + `DUOP_WRONGPATH],
                       fb_duop[2*`DUOP_W + `DUOP_WRONGPATH],
                       fb_duop[1*`DUOP_W + `DUOP_WRONGPATH],
                       fb_duop[0*`DUOP_W + `DUOP_WRONGPATH] };
    wire [2:0] n_wp = {2'b0, ((take > 3'd0) & wpb[0])}
                    + {2'b0, ((take > 3'd1) & wpb[1])}
                    + {2'b0, ((take > 3'd2) & wpb[2])}
                    + {2'b0, ((take > 3'd3) & wpb[3])};
    wire [2:0] take_cp = take - n_wp;

    // ---- wrong-path ----
    wire       any_v   = |fb_valid;
    wire       mispred = any_v & fb_fe_event[7] & ~fb_fe_event[4] & ~in_shadow;
    wire       sh_end  = in_shadow & (shadow_cnt == 6'd0);

    assign fb_take         = take;
    assign fb_redirect     = mispred | sh_end;
    assign fb_redir_shadow = mispred;

    // ---- stall 分類（刻意簡化，只為了讓 counter 有東西可數） ----
    // CONTRACT §8 規則 2：st_fetch + st_refill 必須恰好等於原本的 st_fetch
    wire refilling = in_shadow | (shadow_cnt != 6'd0);
    wire fe_dry    = (nvalid == 3'd0);
    wire st_fetch  = fe_dry & ~refilling;
    wire st_refill = fe_dry &  refilling;
    wire st_rename = rob_full & (want != 3'd0);
    wire st_iq     = (occ >= {1'b0, cfg_iq_entries});
    wire st_rob    = rob_full;
    wire st_lsq    = (fb_mem_event[7:0] != 8'd0);
    wire st_mshr   = fb_fe_event[3];

    always @(posedge clk) begin
        if (rst) begin
            occ <= 7'd0; occ_cp <= 7'd0; in_shadow <= 1'b0; shadow_cnt <= 6'd0;
            c_cycles <= 48'd0; c_retired <= 48'd0; c_wrongpath <= 48'd0;
            c_st_fetch <= 48'd0; c_st_refill <= 48'd0; c_st_rename <= 48'd0;
            c_st_iq <= 48'd0;
            c_st_rob <= 48'd0; c_st_lsq <= 48'd0; c_st_mshr <= 48'd0;
            c_mispred <= 48'd0; c_rob_occ <= 48'd0;
        end else begin
            // flush（後端解析完誤預測）到達時，wrong-path 的部分整批丟掉
            occ    <= sh_end ? (occ_cp + {4'b0, take_cp} - {4'b0, ret})
                             : (occ    + {4'b0, take}    - {4'b0, ret});
            occ_cp <=           occ_cp + {4'b0, take_cp} - {4'b0, ret};

            if (mispred)      in_shadow <= 1'b1;
            else if (sh_end)  in_shadow <= 1'b0;
            else              in_shadow <= in_shadow;

            if (mispred)                 shadow_cnt <= 6'd12;
            else if (shadow_cnt != 6'd0) shadow_cnt <= shadow_cnt - 6'd1;
            else                         shadow_cnt <= 6'd0;

            c_cycles    <= c_cycles    + 48'd1;
            c_retired   <= c_retired   + {45'b0, ret};
            // 綁在 fb_duop 的位元上（不是模型自己的 in_shadow），
            // 這樣這個 counter 就直接驗證 runtime 真的標了 DUOP_WRONGPATH，
            // 而且數字必須精確等於 runtime 供應的 wrong-path 記錄數
            c_wrongpath <= c_wrongpath + {45'b0, n_wp};
            c_st_fetch  <= c_st_fetch  + {47'b0, st_fetch};
            c_st_refill <= c_st_refill + {47'b0, st_refill};
            c_st_rename <= c_st_rename + {47'b0, st_rename};
            c_st_iq     <= c_st_iq     + {47'b0, st_iq};
            c_st_rob    <= c_st_rob    + {47'b0, st_rob};
            c_st_lsq    <= c_st_lsq    + {47'b0, st_lsq};
            c_st_mshr   <= c_st_mshr   + {47'b0, st_mshr};
            c_mispred   <= c_mispred   + {47'b0, mispred};
            c_rob_occ   <= c_rob_occ   + {41'b0, occ};
        end
    end

    assign cnt_cycles      = c_cycles;
    assign cnt_retired     = c_retired;
    assign cnt_wrongpath   = c_wrongpath;
    assign cnt_st_fetch    = c_st_fetch;
    assign cnt_st_refill   = c_st_refill;
    assign cnt_st_rename   = c_st_rename;
    assign cnt_st_iq       = c_st_iq;
    assign cnt_st_rob      = c_st_rob;
    assign cnt_st_lsq      = c_st_lsq;
    assign cnt_st_mshr     = c_st_mshr;
    assign cnt_mispred     = c_mispred;
    assign cnt_rob_occ_sum = c_rob_occ;

    // fb_duop 只用到 slot 0 的 wrongpath 位元
    wire _unused = &{1'b0, fb_duop,
                     cfg_ldq_entries, cfg_stq_entries,
                     cfg_mshr_entries, cfg_issue_width, fb_mem_event[`W*8-1:8],
                     fb_fe_event[6:5], fb_fe_event[2:0], 1'b0};
endmodule
