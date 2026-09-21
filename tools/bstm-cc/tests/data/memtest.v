// 同步讀 / 同步寫的小記憶體（CONTRACT 規則 3：不可非同步讀）
module memtest (
    input             clk,
    input             we,
    input      [3:0]  waddr,
    input      [7:0]  wdata,
    input             re,
    input      [3:0]  raddr,
    output reg [7:0]  rdata,
    output reg [7:0]  shadow
);
    reg [7:0] mem [0:15];
    integer i;
    always @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
        if (re) rdata <= mem[raddr];
        shadow <= mem[0];
    end
endmodule
