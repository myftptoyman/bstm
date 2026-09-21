#!/usr/bin/env python3
"""把真實 OOO 模型（model/ooo/，監督者與 D/E/F agent 維護）複製進 tests/data/ooo
並合成成網表。bstm-cc 只寫自己的目錄，所以一定是複製一份再跑。"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SNAP = os.path.join(HERE, "data", "ooo")
SRC = os.path.abspath(os.path.join(HERE, "..", "..", "..", "model", "ooo"))
IMAGE = os.environ.get("BSTM_YOSYS_IMAGE", "hdlc/yosys:latest")

FILES = ["top.v", "frontend/fe_front.v", "rename/rn_rename.v",
         "backend/be_dispatch.v", "backend/be_iq.v", "backend/be_eu.v",
         "backend/be_rob.v", "lsu/lsu_q.v"]


def sync():
    """把 model/ooo/ 複製一份進 tests/data/ooo（bstm-cc 不寫自己目錄以外的地方）。"""
    if not os.path.isdir(SRC):
        return False
    for f in FILES + ["common/ifc.vh"]:
        dst = os.path.join(SNAP, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(os.path.join(SRC, f), dst)
    return True


def synth():
    script = ("read_verilog -I. " + " ".join(FILES) + "\n"
              "hierarchy -check -top ooo_top\n"
              "proc; opt_expr; opt_clean; flatten; opt -fast\n"
              "write_json ooo_top.json\nstat\n")
    subprocess.run(["docker", "run", "--rm", "-v", "%s:/w" % SNAP, "-w", "/w",
                    IMAGE, "yosys", "-p", script], check=True)
    return os.path.join(SNAP, "ooo_top.json")


if __name__ == "__main__":
    if not sync():
        sys.stderr.write("找不到 %s，沿用既有快照\n" % SRC)
    print(synth())
