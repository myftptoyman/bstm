"""bstm-cc 命令列介面。"""

import argparse
import sys

from . import backend as be
from . import netlist
from .compiler import compile_module
from .errors import BstmError

USAGE = """bstm-cc <netlist.json> <top-module> [--backend c64] [--lanes 64] [-o out.c] [--stats]"""


def build_parser():
    ap = argparse.ArgumentParser(
        prog="bstm-cc",
        description="Yosys word-level 網表 → bit-sliced C（BSTM 編譯鏈，PLAN 第 6 章）",
        usage=USAGE)
    ap.add_argument("netlist", help="yosys write_json 的輸出")
    ap.add_argument("top", nargs="?", default=None,
                    help="top module 名稱（只有一個 module 時可省略）")
    ap.add_argument("--backend", default="c64", choices=sorted(be.BACKENDS),
                    help="目標 word 型別（預設 c64）")
    ap.add_argument("--lanes", type=int, default=None,
                    help="lane 數；必須跟 backend 的天然寬度一致")
    ap.add_argument("-o", "--output", default="-", help="輸出檔（預設 stdout）")
    ap.add_argument("--stats", action="store_true", help="印 net/slot/op 統計到 stderr")
    ap.add_argument("--no-comments", action="store_true",
                    help="不要在產生的 C 裡放 net 名稱註解（檔案比較小）")
    ap.add_argument("--scheduler", default="auto",
                    choices=["auto", "greedy", "dfs"],
                    help="slot 排程啟發式（預設 auto：兩種都跑，取 slot 少的）")
    ap.add_argument("--chunk", type=int, default=1000,
                    help="每個 static 子函式最多幾個 op（0 = 不切；預設 1000）。"
                         "大設計不切的話 gcc 的最佳化會超線性爆炸")
    ap.add_argument("--no-alloc", action="store_true",
                    help="關掉 liveness/slot 配置（每個值一個 slot），用來做效能對照")
    ap.add_argument("-q", "--quiet", action="store_true", help="不要印警告")
    return ap


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = build_parser()
    args = ap.parse_args(argv)
    try:
        m = netlist.load(args.netlist, args.top)
        d = compile_module(m, scheduler=args.scheduler)
        if args.no_alloc:
            disable_alloc(d)
        backend = be.make(args.backend)
        em = be.Emitter(d, backend, lanes=args.lanes,
                        comments=not args.no_comments, chunk=args.chunk)
        text = em.emit()
    except BstmError as e:
        sys.stderr.write("bstm-cc: 錯誤: %s\n" % e)
        return 2

    if args.output == "-":
        sys.stdout.write(text)
    else:
        with open(args.output, "w") as f:
            f.write(text)

    if not args.quiet:
        for w in d.warnings[:20]:
            sys.stderr.write("bstm-cc: 警告: %s\n" % w)
        if len(d.warnings) > 20:
            sys.stderr.write("bstm-cc: 警告: （還有 %d 則）\n" % (len(d.warnings) - 20))
    if args.stats:
        sys.stderr.write(format_stats(d, backend))
    return 0


def disable_alloc(d):
    """對照組：每個 op 一個獨立 slot（等同原型「每個 net 一個區域變數」）。"""
    n = 0
    for ins in d.stream:
        if ins.op == "op":
            ins.slot = n
            d.slot_of[ins.value] = n
            n += 1
    d.nslots = n
    d.stats["slots"] = n


def format_stats(d, backend):
    st = d.stats
    ratio = (float(st["nets"]) / st["slots"]) if st["slots"] else float("inf")
    lines = [
        "---- bstm-cc 統計 (%s) ----" % d.name,
        "  yosys cell        : %d" % st["cells"],
        "  net bit           : %d" % st["nets"],
        "  state bit (FF/mem): %d" % st["state_bits"],
        "  IR op 建立        : %d" % st["ops_built"],
        "  IR op 存活 (DCE後): %d" % st["ops_live"],
        "  slot              : %d  (排程器 %s)" % (st["slots"], st.get("scheduler", "?")),
        "  壓縮比 net→slot   : %.1fx" % ratio,
        "  工作集 (slot)     : %d slot x %d lane = %d bytes"
        % (st["slots"], backend.lanes, st["slots"] * backend.lanes // 8),
        "  state 大小        : %d bytes"
        % (st["state_bits"] * backend.lanes // 8),
    ]
    return "\n".join(lines) + "\n"
