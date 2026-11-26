"""Restricted LLVM intrinsic control/data-flow validation (trusted intrinsic semantics)."""
import argparse
from pathlib import Path
import re
from llvmlite import binding as llvm

IDENT = r'[-a-zA-Z$._0-9]+'
OPS = {'add': 'sadd', 'sub': 'ssub', 'mul': 'smul', 'neg': 'ssub'}


def validate(ir, name, op):
    def require(ok):
        if not ok:
            raise ValueError('unsupported checked intrinsic IR')
    require(op in OPS and len(ir.encode('utf-8')) <= 262144)
    require(re.fullmatch(IDENT, name) is not None)
    module = llvm.parse_assembly(ir)
    module.verify()
    def function(symbol):
        try:
            return module.get_function(symbol)
        except NameError as e:
            raise ValueError('missing required function: ' + symbol) from e
    fn = function(name)
    args, blocks = list(fn.arguments), list(fn.blocks)
    n = 1 if op == 'neg' else 2
    require(str(fn.global_value_type) == ('i64 (i64)' if n == 1 else 'i64 (i64, i64)'))
    require(len(args) == n and len(blocks) == 3)
    require(not list(fn.attributes) and all(not list(a.attributes) for a in args))
    require(re.fullmatch(r'define (?:internal )?i64 @' + re.escape(name) + r'\(' + ', '.join([r'i64 %' + IDENT]*n) + r'\) \{', str(fn).splitlines()[0]) is not None)
    require(all('!' not in str(i) for b in blocks for i in b.instructions))
    def rhs(i):
        m = re.fullmatch(r'%' + IDENT + r' = (.+)', str(i).strip())
        require(m is not None)
        return m.group(1)
    vals = {a: k for k, a in enumerate(args)}
    slots = {}
    entry = list(blocks[0].instructions)
    require(4 <= len(entry) <= 32)
    pair, value, overflow, branch = entry[-4:]
    require([i.opcode for i in (pair,value,overflow,branch)] == ['call','extractvalue','extractvalue','br'])
    for i in entry[:-4]:
        o = list(i.operands)
        if i.opcode == 'alloca':
            require(rhs(i) == 'alloca i64, align 8' and len(o)==1 and o[0].get_constant_value()==1)
            slots[i] = None
        elif i.opcode == 'store':
            require(len(o)==2 and o[0] in vals and o[1] in slots and slots[o[1]] is None)
            require(re.fullmatch(r'store i64 %' + IDENT + r', ptr %' + IDENT + r', align (?:4|8)', str(i).strip()) is not None)
            slots[o[1]] = vals[o[0]]
        elif i.opcode == 'load':
            require(len(o)==1 and o[0] in slots and slots[o[0]] is not None)
            require(re.fullmatch(r'load i64, ptr %' + IDENT + r', align (?:4|8)', rhs(i)) is not None)
            vals[i] = slots[o[0]]
        else:
            raise ValueError('unexpected entry instruction')
    intrinsic = 'llvm.' + OPS[op] + '.with.overflow.i64'
    decl = function(intrinsic)
    require(decl.is_declaration and str(decl.global_value_type) == '{ i64, i1 } (i64, i64)')
    operands = list(pair.operands)
    require(len(operands)==3 and operands[-1]==decl)
    if op == 'neg':
        require(operands[0].value_kind.name=='constant_int' and operands[0].get_constant_value()==0 and vals.get(operands[1])==0)
        left = '0'
    else:
        require(vals.get(operands[0])==0 and vals.get(operands[1])==1)
        left = '%' + IDENT
    require(re.fullmatch(r'call \{ i64, i1 \} @' + re.escape(intrinsic) + r'\(i64 ' + left + r', i64 %' + IDENT + r'\)', rhs(pair)) is not None)
    for inst,index,ty in [(value,0,'i64'),(overflow,1,'i1')]:
        require(list(inst.operands)==[pair] and str(inst.type)==ty)
        require(re.fullmatch(r'extractvalue \{ i64, i1 \} %' + IDENT + ', ' + str(index), rhs(inst)) is not None)
    c = list(branch.operands)
    require(len(c)==3 and c[0]==overflow and c[1]!=c[2] and set(c[1:])==set(blocks[1:]))
    require(re.fullmatch(r'br i1 %'+IDENT+r', label %'+IDENT+r', label %'+IDENT,str(branch).strip()) is not None)
    bad, good = (next(b for b in blocks if b==x) for x in (c[2],c[1]))
    fail, ok = list(bad.instructions), list(good.instructions)
    require([str(i).strip() for i in fail]==['call void @llvm.trap()','unreachable'])
    require(function('llvm.trap').is_declaration)
    require(len(ok)==1 and ok[0].opcode=='ret' and list(ok[0].operands)==[value])
    require(re.fullmatch(r'ret i64 %'+IDENT,str(ok[0]).strip()) is not None)
    return op


def main():
    p=argparse.ArgumentParser(); p.add_argument('ir',type=Path); p.add_argument('symbol'); p.add_argument('operation',choices=OPS)
    a=p.parse_args()
    with a.ir.open('rb') as f: data=f.read(262145)
    validate(data.decode('utf-8'),a.symbol,a.operation)
    print('RESTRICTED_INTRINSIC_FLOW_VALIDATED',a.operation,a.symbol)

if __name__=='__main__': main()
