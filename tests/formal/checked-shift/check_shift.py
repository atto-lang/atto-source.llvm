"""Restricted i64 shift IR control/data-flow validator (trusted LLVM semantics)."""
import argparse
from pathlib import Path
import re
from llvmlite import binding as llvm

ID = r'[-a-zA-Z$._0-9]+'
OPS = ('shl', 'lshr', 'ashr')


def validate(ir, name, op):
    def require(ok):
        if not ok:
            raise ValueError('unsupported checked shift IR')
    require(op in OPS and len(ir.encode('utf-8')) <= 262144)
    require(re.fullmatch(ID, name) is not None)
    module = llvm.parse_assembly(ir)
    module.verify()
    try:
        fn = module.get_function(name)
        trap = module.get_function('llvm.trap')
    except NameError as e:
        raise ValueError('missing function') from e
    args, blocks = list(fn.arguments), list(fn.blocks)
    require(str(fn.global_value_type) == 'i64 (i64, i64)' and len(args)==2 and len(blocks)==3)
    require(not list(fn.attributes) and all(not list(a.attributes) for a in args))
    require(re.fullmatch(r'define (?:internal )?i64 @'+re.escape(name)+r'\(i64 %'+ID+r', i64 %'+ID+r'\) \{', str(fn).splitlines()[0]) is not None)
    require(all('!' not in str(i) for b in blocks for i in b.instructions))
    require(trap.is_declaration and str(trap.global_value_type)=='void ()')
    def rhs(i):
        m = re.fullmatch(r'%'+ID+r' = (.+)',str(i).strip())
        require(m is not None)
        return m.group(1)
    vals = {a:k for k,a in enumerate(args)}
    slots = {}
    entry = list(blocks[0].instructions)
    require(2<=len(entry)<=32)
    cmp, branch = entry[-2:]
    for i in entry[:-2]:
        o=list(i.operands)
        if i.opcode=='alloca':
            require(rhs(i)=='alloca i64, align 8' and len(o)==1 and o[0].get_constant_value()==1)
            slots[i]=None
        elif i.opcode=='store':
            require(len(o)==2 and o[0] in vals and o[1] in slots and slots[o[1]] is None)
            require(re.fullmatch(r'store i64 %'+ID+r', ptr %'+ID+r', align (?:4|8)',str(i).strip()) is not None)
            slots[o[1]]=vals[o[0]]
        elif i.opcode=='load':
            require(len(o)==1 and o[0] in slots and slots[o[0]] is not None)
            require(re.fullmatch(r'load i64, ptr %'+ID+r', align (?:4|8)',rhs(i)) is not None)
            vals[i]=slots[o[0]]
        else:
            raise ValueError('unexpected entry instruction')
    c=list(cmp.operands)
    require(cmp.opcode=='icmp' and len(c)==2 and vals.get(c[0])==1 and c[1].value_kind.name=='constant_int' and c[1].get_constant_value()==64)
    require(re.fullmatch(r'icmp ult i64 %'+ID+r', 64',rhs(cmp)) is not None)
    b=list(branch.operands)
    require(branch.opcode=='br' and len(b)==3 and b[0]==cmp and b[1]!=b[2] and set(b[1:])==set(blocks[1:]))
    require(re.fullmatch(r'br i1 %'+ID+r', label %'+ID+r', label %'+ID,str(branch).strip()) is not None)
    # LLVM C API enumerates conditional branch successors false, then true.
    bad,good=(next(x for x in blocks if x==v) for v in b[1:])
    require([str(i).strip() for i in bad.instructions]==['call void @llvm.trap()','unreachable'])
    ok=list(good.instructions)
    require(len(ok)==2 and ok[0].opcode==op and ok[1].opcode=='ret')
    shift,ret=ok
    o=list(shift.operands)
    require(len(o)==2 and vals.get(o[0])==0 and vals.get(o[1])==1)
    require(re.fullmatch(op+r' i64 %'+ID+r', %'+ID,rhs(shift)) is not None)
    require(list(ret.operands)==[shift] and re.fullmatch(r'ret i64 %'+ID,str(ret).strip()) is not None)
    return op


def main():
    p=argparse.ArgumentParser();p.add_argument('ir',type=Path);p.add_argument('symbol');p.add_argument('operation',choices=OPS);a=p.parse_args()
    with a.ir.open('rb') as f: data=f.read(262145)
    validate(data.decode('utf-8'),a.symbol,a.operation)
    print('RESTRICTED_SHIFT_FLOW_VALIDATED',a.operation,a.symbol)

if __name__=='__main__': main()
