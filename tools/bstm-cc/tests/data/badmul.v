// 故意用乘法（違反建模規則 1），用來測錯誤訊息
module badmul (input clk, input [7:0] a, input [7:0] b, output reg [15:0] y);
    always @(posedge clk) y <= a * b;
endmodule
