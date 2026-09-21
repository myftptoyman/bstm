#!/usr/bin/env python3
"""Yosys JSON -> bit-sliced C.  每個 net 一個 W-bit word，W 條 lane = W 個獨立 instance。"""
import json, sys

W = "vec_t"   # uint64_t / uint32x4_t / Metal uint4 ...

def load(path, top):
    m = json.load(open(path))['modules'][top]
    return m

def netname(b):
    if b == '0': return "VZERO"
    if b == '1': return "VONES"
    if b == 'x': return "VZERO"
    return f"n{b}"

def bits(conn):          # conn 是 bit list，LSB first
    return [netname(b) for b in conn]

def emit(m, out):
    P = lambda s: out.append(s)
    # 收集所有 net id
    nets = set()
    for c in m['cells'].values():
        for conn in c['connections'].values():
            for b in conn:
                if isinstance(b, int): nets.add(b)
    for p in m['ports'].values():
        for b in p['bits']:
            if isinstance(b, int): nets.add(b)

    P("typedef uint64_t vec_t;        /* 64 lanes = 64 instances */")
    P("#define VZERO ((vec_t)0)")
    P("#define VONES (~(vec_t)0)")
    P("")
    P("typedef struct {   /* 所有 DFF 的狀態，每個 bit 一個 word */")
    ffbits = []
    for n,c in m['cells'].items():
        if c['type'].startswith('$sdff') or c['type'].startswith('$dff'):
            for b in c['connections']['Q']:
                if isinstance(b,int): ffbits.append(b)
    for b in sorted(set(ffbits)):
        P(f"    vec_t q{b};")
    P("} state_t;")
    P("")
    P("static void eval_cycle(state_t *s, const state_t *p,")
    P("                       vec_t rst, vec_t in_valid, const vec_t in_tag[6],")
    P("                       const vec_t in_lat[4], vec_t dn_credit_ret,")
    P("                       vec_t *in_ready, vec_t *out_valid, vec_t out_tag[6])")
    P("{")
    for b in sorted(nets):
        P(f"    vec_t n{b};")
    P("")
    P("    /* --- FF 輸出 = 上一 cycle 的狀態 --- */")
    for b in sorted(set(ffbits)):
        P(f"    n{b} = p->q{b};")
    P("")
    P("    /* --- 輸入接腳 --- */")
    pin = m['ports']
    P(f"    n{pin['rst']['bits'][0]} = rst;")
    P(f"    n{pin['in_valid']['bits'][0]} = in_valid;")
    for i,b in enumerate(pin['in_tag']['bits']):   P(f"    n{b} = in_tag[{i}];")
    for i,b in enumerate(pin['in_lat']['bits']):   P(f"    n{b} = in_lat[{i}];")
    P(f"    n{pin['dn_credit_ret']['bits'][0]} = dn_credit_ret;")
    P("")
    P("    /* --- 組合邏輯（已依拓樸順序排好） --- */")

    # 拓樸排序：輸出 net 依賴輸入 net
    IN  = {'$and':['A','B'],'$or':['A','B'],'$xor':['A','B'],'$not':['A'],
           '$mux':['A','B','S'],'$pmux':['A','B','S'],'$eq':['A','B'],
           '$ne':['A','B'],'$logic_not':['A'],'$reduce_bool':['A'],
           '$add':['A','B'],'$sub':['A','B']}
    seq, done, pend = [], set(b for b in sorted(set(ffbits))), dict(m['cells'])
    pend = {k:v for k,v in pend.items() if not v['type'].startswith('$sdff') and not v['type'].startswith('$dff')}
    for b in nets:
        pass
    driven = set()
    for c in pend.values():
        for b in c['connections'].get('Y',[]):
            if isinstance(b,int): driven.add(b)
    ready = set(b for b in nets if b not in driven) | set(ffbits)
    while pend:
        prog = False
        for n,c in list(pend.items()):
            need = []
            for port in IN.get(c['type'],[]):
                need += [b for b in c['connections'].get(port,[]) if isinstance(b,int)]
            if all(b in ready for b in need):
                seq.append((n,c)); 
                for b in c['connections'].get('Y',[]):
                    if isinstance(b,int): ready.add(b)
                del pend[n]; prog = True
        if not prog:
            P(f"    /* !! 組合迴圈，剩 {len(pend)} 個 cell */"); break

    for n,c in seq:
        t, cn = c['type'], c['connections']
        Y = bits(cn.get('Y',[]))
        if t == '$and':
            A,B = bits(cn['A']), bits(cn['B'])
            for i in range(len(Y)): P(f"    {Y[i]} = {A[i]} & {B[i]};")
        elif t == '$or':
            A,B = bits(cn['A']), bits(cn['B'])
            for i in range(len(Y)): P(f"    {Y[i]} = {A[i]} | {B[i]};")
        elif t == '$xor':
            A,B = bits(cn['A']), bits(cn['B'])
            for i in range(len(Y)): P(f"    {Y[i]} = {A[i]} ^ {B[i]};")
        elif t == '$not':
            A = bits(cn['A'])
            for i in range(len(Y)): P(f"    {Y[i]} = ~{A[i]};")
        elif t == '$mux':
            A,B,S = bits(cn['A']), bits(cn['B']), bits(cn['S'])[0]
            for i in range(len(Y)): P(f"    {Y[i]} = ({A[i]} & ~{S}) | ({B[i]} & {S});   /* mux */")
        elif t in ('$reduce_bool',):     # OR-reduce
            A = bits(cn['A'])
            P(f"    {Y[0]} = " + " | ".join(A) + ";   /* reduce_bool (!=0) */")
        elif t == '$logic_not':          # NOR-reduce  (== 0)
            A = bits(cn['A'])
            P(f"    {Y[0]} = ~(" + " | ".join(A) + ");   /* logic_not (==0) */")
        elif t == '$eq':
            A,B = bits(cn['A']), bits(cn['B'])
            terms = " | ".join(f"({A[i]} ^ {B[i]})" for i in range(len(A)))
            P(f"    {Y[0]} = ~({terms});   /* eq */")
        elif t in ('$add','$sub'):
            A,B = bits(cn['A']), bits(cn['B'])
            B = B + ["VZERO"]*(len(Y)-len(B))
            inv = "~" if t=='$sub' else ""
            P(f"    {{  /* {t[1:]}: ripple carry, {len(Y)} bit */")
            P(f"        vec_t c = {'VONES' if t=='$sub' else 'VZERO'};")
            for i in range(len(Y)):
                b = f"{inv}{B[i]}" if B[i]!="VZERO" else ("VONES" if t=='$sub' else "VZERO")
                P(f"        {{ vec_t a={A[i]}, b={b};")
                P(f"          {Y[i]} = a ^ b ^ c;")
                P(f"          c = (a&b) | (c&(a^b)); }}")
            P("    }")
        elif t == '$pmux':
            A,B,S = bits(cn['A']), bits(cn['B']), bits(cn['S'])
            w = len(Y)
            P(f"    {{  /* pmux: {len(S)} 路 one-hot */")
            for i in range(w):
                terms = " | ".join(f"({B[j*w+i]} & {S[j]})" for j in range(len(S)))
                P(f"        {Y[i]} = {terms};")
            P("    }")
        else:
            P(f"    /* TODO cell type {t} */")
    P("")
    P("    /* --- FF 更新（同步 reset + enable） --- */")
    for n,c in m['cells'].items():
        if not (c['type'].startswith('$sdff') or c['type'].startswith('$dff')): continue
        cn = c['connections']
        D  = bits(cn['D']); Q = [b for b in cn['Q']]
        EN = bits(cn['EN'])[0] if 'EN' in cn else "VONES"
        SR = bits(cn['SRST'])[0] if 'SRST' in cn else None
        for i,q in enumerate(Q):
            e = f"({EN} & ~{SR})" if SR else EN
            rv = "VZERO"
            P(f"    s->q{q} = ({D[i]} & {e}) | (p->q{q} & ~{e}" + (f" & ~{SR}" if SR else "") + ");")
    P("")
    P("    /* --- 輸出接腳 --- */")
    P(f"    *in_ready  = n{pin['in_ready']['bits'][0]};")
    P(f"    *out_valid = n{pin['out_valid']['bits'][0]};")
    for i,b in enumerate(pin['out_tag']['bits']): P(f"    out_tag[{i}] = n{b};")
    P("}")

out=[]
emit(load(sys.argv[1], sys.argv[2]), out)
print("\n".join(out))
