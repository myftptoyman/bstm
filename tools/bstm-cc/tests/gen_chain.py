#!/usr/bin/env python3
"""產生 N 級 stage 串接的 chain.v，用來做規模測試。

prototype/chain.v 是 32 級（815 cell）。真實 OOO timing model 估計 ~26,000 cell
（PLAN §10.2），所以規模測試用 800 級（~20,000 cell）。
"""
import sys


def gen(n, top="chain"):
    L = []
    L.append("module %s(input clk, input rst, input in_valid, input [5:0] in_tag," % top)
    L.append("             input [3:0] in_lat, input dn_credit_ret,")
    L.append("             output in_ready, output out_valid, output [5:0] out_tag);")
    for i in range(n):
        L.append("  wire v%d, r%d; wire [5:0] t%d;" % (i, i, i))
    for i in range(n):
        src_v = "in_valid" if i == 0 else "v%d" % (i - 1)
        src_t = "in_tag" if i == 0 else "t%d" % (i - 1)
        L.append("  stage u%d(.clk(clk),.rst(rst),.in_valid(%s),.in_tag(%s),"
                 ".in_lat(in_lat ^ 4'd%d),.dn_credit_ret(dn_credit_ret),"
                 ".in_ready(r%d),.out_valid(v%d),.out_tag(t%d));"
                 % (i, src_v, src_t, i % 16, i, i, i))
    L.append("  assign in_ready  = r0;")
    L.append("  assign out_valid = v%d;" % (n - 1))
    L.append("  assign out_tag   = t%d;" % (n - 1))
    L.append("endmodule")
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 800
    sys.stdout.write(gen(n))
