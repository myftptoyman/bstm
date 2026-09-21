#!/usr/bin/env python3
"""用 docker hdlc/yosys 把 .v 合成成 word-level JSON（PLAN §15.1 的指令）。"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
IMAGE = os.environ.get("BSTM_YOSYS_IMAGE", "hdlc/yosys:latest")

SCRIPT = """
read_verilog {files}
hierarchy -check -top {top}
proc; opt_expr; opt_clean; flatten; opt -fast
{extra}
write_json {out}
stat
"""


def have_yosys():
    if os.environ.get("BSTM_NO_DOCKER"):
        return False
    try:
        subprocess.run(["docker", "image", "inspect", IMAGE],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       check=True)
        return True
    except Exception:
        return False


def synth(vfiles, top, out=None, extra="", cwd=DATA):
    """回傳產生的 json 路徑。vfiles 是相對 cwd 的檔名。"""
    out = out or (top + ".json")
    script = SCRIPT.format(files=" ".join(vfiles), top=top, out=out, extra=extra)
    outpath = os.path.join(cwd, out)
    if os.path.exists(outpath):
        newest = max(os.path.getmtime(os.path.join(cwd, f)) for f in vfiles)
        if os.path.getmtime(outpath) > newest:
            return outpath
    subprocess.run(["docker", "run", "--rm", "-v", "%s:/w" % cwd, "-w", "/w",
                    IMAGE, "yosys", "-p", script],
                   check=True, stdout=subprocess.DEVNULL)
    return outpath


if __name__ == "__main__":
    print(synth(sys.argv[2:], sys.argv[1]))
