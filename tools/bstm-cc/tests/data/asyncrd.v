// 故意寫成非同步讀，用來測 bstm-cc 會不會明確報錯
module asyncrd (
    input             clk,
    input             we,
    input      [3:0]  waddr,
    input      [7:0]  wdata,
    input      [3:0]  raddr,
    output     [7:0]  rdata
);
    reg [7:0] mem [0:15];
    always @(posedge clk) if (we) mem[waddr] <= wdata;
    assign rdata = mem[raddr];
endmodule
