// 一個很典型的 timing model 片段：
// 一級 pipeline stage + valid/stall 握手 + credit counter（下游有幾個空位）
module stage #(
    parameter CREDIT_W = 4
)(
    input  wire                clk,
    input  wire                rst,
    input  wire                in_valid,
    input  wire [5:0]          in_tag,      // 指令 tag，不是資料
    input  wire [3:0]          in_lat,      // ISS 給的延遲 class
    input  wire                dn_credit_ret, // 下游還一個 credit
    output wire                in_ready,
    output wire                out_valid,
    output wire [5:0]          out_tag
);
    reg                 v;
    reg [5:0]           tag;
    reg [3:0]           cnt;      // 剩餘延遲倒數
    reg [CREDIT_W-1:0]  credit;

    wire busy      = v & (cnt != 4'd0);
    wire can_issue = v & (cnt == 4'd0) & (credit != 0);
    wire accept    = in_valid & ~v;

    assign in_ready  = ~v;
    assign out_valid = can_issue;
    assign out_tag   = tag;

    always @(posedge clk) begin
        if (rst) begin
            v <= 1'b0; tag <= 6'd0; cnt <= 4'd0;
            credit <= {CREDIT_W{1'b1}};
        end else begin
            if (accept) begin
                v   <= 1'b1;
                tag <= in_tag;
                cnt <= in_lat;
            end else if (can_issue) begin
                v   <= 1'b0;
            end

            if (busy)
                cnt <= cnt - 1'b1;

            // credit：發出去扣一個，回收加一個
            case ({can_issue, dn_credit_ret})
                2'b10: credit <= credit - 1'b1;
                2'b01: credit <= credit + 1'b1;
                default: credit <= credit;
            endcase
        end
    end
endmodule
