// bstm-cc 的 cell 覆蓋測試：盡量把 dispatch table 裡的算子都逼出來。
// 全部同步、單一 clock、所有 case 有 default —— 符合 CONTRACT 的五條硬規則。
module ops (
    input              clk,
    input              rst,
    input              en,
    input       [7:0]  a,
    input       [7:0]  b,
    input       [2:0]  sh,
    input       [1:0]  msel,
    input              s,
    output reg  [7:0]  r_add,
    output reg  [7:0]  r_sub,
    output reg  [7:0]  r_neg,
    output reg  [7:0]  r_shl,
    output reg  [7:0]  r_shr,
    output reg  [7:0]  r_sshr,
    output reg  [7:0]  r_mux,
    output reg  [7:0]  r_case,
    output reg  [7:0]  r_and,
    output reg  [7:0]  r_or,
    output reg  [7:0]  r_xor,
    output reg  [7:0]  r_not,
    output reg         r_eq,
    output reg         r_ne,
    output reg         r_ltu,
    output reg         r_leu,
    output reg         r_gtu,
    output reg         r_geu,
    output reg         r_lts,
    output reg         r_ges,
    output reg         r_gts,
    output reg         r_les,
    output reg         r_rand,
    output reg         r_ror,
    output reg         r_rxor,
    output reg         r_land,
    output reg         r_lor,
    output reg         r_lnot,
    output reg         r_bool,
    output reg  [7:0]  r_dff,
    output reg  [7:0]  r_dffe,
    output reg  [7:0]  r_sdff,
    output reg  [7:0]  r_sdffe,
    output reg  [7:0]  r_sdffce
);
    wire signed [7:0] sa = a;
    wire signed [7:0] sb = b;

    always @(posedge clk) begin
        r_add  <= a + b;
        r_sub  <= a - b;
        r_neg  <= -a;
        r_shl  <= a << sh;
        r_shr  <= a >> sh;
        r_sshr <= sa >>> sh;
        r_mux  <= s ? a : b;
        r_and  <= a & b;
        r_or   <= a | b;
        r_xor  <= a ^ b;
        r_not  <= ~a;
        r_eq   <= (a == b);
        r_ne   <= (a != b);
        r_ltu  <= (a <  b);
        r_leu  <= (a <= b);
        r_gtu  <= (a >  b);
        r_geu  <= (a >= b);
        r_lts  <= (sa <  sb);
        r_ges  <= (sa >= sb);
        r_gts  <= (sa >  sb);
        r_les  <= (sa <= sb);
        r_rand <= &a;
        r_ror  <= |a;
        r_rxor <= ^a;
        r_land <= (a && b);
        r_lor  <= (a || b);
        r_lnot <= !a;
        r_bool <= (a != 8'd0);
        case (msel)
            2'd0:    r_case <= a;
            2'd1:    r_case <= b;
            2'd2:    r_case <= a ^ b;
            default: r_case <= 8'hA5;
        endcase
    end

    // $dff（無 enable、無 reset）
    always @(posedge clk) r_dff <= a;
    // $dffe
    always @(posedge clk) if (en) r_dffe <= b;
    // $sdff（同步 reset，reset 值非 0）
    always @(posedge clk) begin
        if (rst) r_sdff <= 8'hA5;
        else     r_sdff <= a + 8'd1;
    end
    // $sdffe（reset 優先於 enable）
    always @(posedge clk) begin
        if (rst)      r_sdffe <= 8'h3C;
        else if (en)  r_sdffe <= b - 8'd1;
    end
    // $sdffce（enable 優先於 reset）
    always @(posedge clk) begin
        if (en) begin
            if (rst) r_sdffce <= 8'h0F;
            else     r_sdffce <= a ^ b;
        end
    end
endmodule
