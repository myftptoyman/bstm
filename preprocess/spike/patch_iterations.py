#!/usr/bin/env python3
"""
patch_iterations.py -- make a private copy of the CoreMark ELF with a
different iteration count.

CoreMark reads its iteration count from `seed4_volatile` (core_main.c does
`results[0].iterations = get_seed_32(4)`).  The Andes bare-metal build has
that variable in .data, so we can simply rewrite the initialiser in a copy
of the ELF -- no toolchain and no source tree needed.

The original ELF in /home/toyman/work/mc_run_cmv5/ is never modified.
"""
import argparse, shutil, struct, sys
from pathlib import Path

def elf_sections(data):
    assert data[:4] == b'\x7fELF' and data[4] == 2, "not an ELF64"
    e_shoff   = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsz = struct.unpack_from('<H', data, 0x3a)[0]
    e_shnum   = struct.unpack_from('<H', data, 0x3c)[0]
    e_shstrndx= struct.unpack_from('<H', data, 0x3e)[0]
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsz
        name, typ, flags, addr, off, size = struct.unpack_from('<IIQQQQ', data, o)
        secs.append(dict(name_off=name, type=typ, addr=addr, off=off, size=size))
    stro = secs[e_shstrndx]['off']
    for s in secs:
        e = data.index(b'\0', stro + s['name_off'])
        s['name'] = data[stro + s['name_off']:e].decode()
    return secs

def find_symbol(data, secs, want):
    symtab = next((s for s in secs if s['name'] == '.symtab'), None)
    strtab = next((s for s in secs if s['name'] == '.strtab'), None)
    if not symtab or not strtab:
        return None
    n = symtab['size'] // 24
    for i in range(n):
        o = symtab['off'] + i * 24
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from('<IBBHQQ', data, o)
        e = data.index(b'\0', strtab['off'] + st_name)
        nm = data[strtab['off'] + st_name:e].decode()
        if nm == want:
            return st_value, st_size
    return None

def addr_to_off(secs, addr):
    for s in secs:
        if s['type'] != 8 and s['size'] and s['addr'] <= addr < s['addr'] + s['size']:
            return s['off'] + (addr - s['addr'])
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--iterations', type=int, required=True)
    ap.add_argument('--symbol', default='seed4_volatile')
    a = ap.parse_args()

    data = bytearray(Path(a.src).read_bytes())
    secs = elf_sections(data)
    sym = find_symbol(data, secs, a.symbol)
    if not sym:
        sys.exit(f"symbol {a.symbol} not found in {a.src}")
    addr, size = sym
    off = addr_to_off(secs, addr)
    if off is None:
        sys.exit(f"{a.symbol} @ 0x{addr:x} is not inside a loadable section with file data")
    old = struct.unpack_from('<I', data, off)[0]
    struct.pack_into('<I', data, off, a.iterations)
    Path(a.dst).write_bytes(bytes(data))
    Path(a.dst).chmod(0o755)
    print(f"{a.symbol} @ 0x{addr:x} (file off 0x{off:x}): {old} -> {a.iterations}")
    print(f"wrote {a.dst}")

if __name__ == '__main__':
    main()
