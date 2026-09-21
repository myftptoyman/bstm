"""Yosys write_json 的 word-level 網表解析。"""

import json

from .errors import BstmError

CONST_CHARS = {"0": 0, "1": 1, "x": 0, "z": 0, "-": 0}


def pint(cell, key, default=None):
    """把 yosys 的參數讀成 Python int。"""
    p = cell.get("parameters") or {}
    if key not in p:
        if default is None:
            raise BstmError("cell %s 缺少參數 %s" % (cell.get("type"), key))
        return default
    v = p[key]
    if isinstance(v, int):
        return v
    s = str(v)
    if s == "":
        return 0
    if all(c in CONST_CHARS for c in s):
        return int("".join(str(CONST_CHARS[c]) for c in s), 2)
    raise BstmError("參數 %s 不是數值: %r" % (key, v))


def pbits(cell, key, width, default=0):
    """把 yosys 的常數參數讀成 LSB-first 的 0/1 list。"""
    p = cell.get("parameters") or {}
    if key not in p:
        return [(default >> i) & 1 for i in range(width)]
    v = p[key]
    if isinstance(v, int):
        return [(v >> i) & 1 for i in range(width)]
    s = str(v)
    bits = [CONST_CHARS.get(c, 0) for c in reversed(s)]
    bits += [0] * max(0, width - len(bits))
    return bits[:width]


class Port(object):
    __slots__ = ("name", "direction", "bits")

    def __init__(self, name, direction, bits):
        self.name = name
        self.direction = direction
        self.bits = bits

    @property
    def width(self):
        return len(self.bits)


class Module(object):
    def __init__(self, name, raw):
        self.name = name
        self.raw = raw
        self.ports = []
        for pn, pv in (raw.get("ports") or {}).items():
            bits = list(pv["bits"])
            if pv.get("upto"):
                bits = list(reversed(bits))
            d = pv.get("direction", "input")
            if d == "inout":
                raise BstmError("不支援 inout 埠: %s" % pn)
            self.ports.append(Port(pn, d, bits))
        self.cells = dict(raw.get("cells") or {})
        self.netnames = dict(raw.get("netnames") or {})
        self.memories = dict(raw.get("memories") or {})

    def inputs(self):
        return [p for p in self.ports if p.direction == "input"]

    def outputs(self):
        return [p for p in self.ports if p.direction == "output"]

    def all_net_bits(self):
        """設計裡出現過的所有整數 net id。"""
        nets = set()
        for c in self.cells.values():
            for conn in c["connections"].values():
                for b in conn:
                    if isinstance(b, int):
                        nets.add(b)
        for p in self.ports:
            for b in p.bits:
                if isinstance(b, int):
                    nets.add(b)
        return nets

    def net_debug_names(self):
        """net id -> 人看得懂的名字（取第一個 public net）。"""
        out = {}
        for nn, nv in self.netnames.items():
            if nv.get("hide_name"):
                continue
            bits = list(nv["bits"])
            if nv.get("upto"):
                bits = list(reversed(bits))
            off = nv.get("offset", 0)
            for i, b in enumerate(bits):
                if isinstance(b, int) and b not in out:
                    out[b] = nn if len(bits) == 1 else "%s[%d]" % (nn, i + off)
        return out


def load(path, top=None):
    with open(path) as f:
        doc = json.load(f)
    mods = doc.get("modules") or {}
    if not mods:
        raise BstmError("%s 裡沒有任何 module" % path)
    if top is None:
        tops = [k for k, v in mods.items()
                if (v.get("attributes") or {}).get("top") in (1, "1", "00000000000000000000000000000001")]
        if len(tops) == 1:
            top = tops[0]
        elif len(mods) == 1:
            top = list(mods)[0]
        else:
            raise BstmError("有多個 module，請用位置參數指定 top：%s" % ", ".join(sorted(mods)))
    if top not in mods:
        raise BstmError("找不到 module %r（可用的有：%s）" % (top, ", ".join(sorted(mods))))
    return Module(top, mods[top])
