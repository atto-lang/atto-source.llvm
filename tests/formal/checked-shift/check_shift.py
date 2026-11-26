"""Restricted scalar shift IR validation; trusts LLVM instruction semantics."""
import argparse
import re
from pathlib import Path
from llvmlite import binding as llvm

IDENT = r'[-a-zA-Z$._0-9]+'
OPS = {'left': 'shl', 'logical-right': 'lshr', 'arithmetic-right': 'ashr'}


def validate(ir, name, operation):
    def require(ok):
        if not ok:
            raise ValueError('unsupported checked shift IR')
    require(operation in OPS and len(ir.encode('utf-8')) <= 262144)
    require(re.fullmatch(IDENT, name) is not None)
    module = llvm.parse_assembly(ir)
    module.verify()
    fn = module.get_function(name)
    args, blocks = list(fn.arguments), list(fn.blocks)
    require(str(fn.global_value_type) == 'i64 (i64, i64)' and len(blocks) == 3)
    require(not list(fn.attributes) and all(not list(a.attributes) for a in args))
    require(re.fullmatch(r'define (?:internal )?i64 @' + re.escape(name) + r'\(i64 %' + IDENT + r', i64 %' + IDENT + r'\) \{', str(fn).splitlines()[0]) is not None)
    require(all('!' not in str(i) for b in blocks for i in b.instructions))
    def rhs(i):
        m = re.fullmatch(r'%' + IDENT + r' = (.+)', str(i).strip())
        require(m is not None)
        return m.group(1)
    values, slots = dict(zip(args, ('a', 'n'))), {}
    entry = list(blocks[0].instructions)
    require(2 <= len(entry) <= 32)
    guard, branch = entry[-2:]
    require(guard.opcode == 'icmp' and branch.opcode == 'br')
    for i in entry[:-2]:
        o = list(i.operands)
        if i.opcode == 'alloca':
            require(rhs(i) == 'alloca i64, align 8' and len(o) == 1 and o[0].get_constant_value() == 1)
            slots[i] = None
        elif i.opcode == 'store':
            require(len(o) == 2 and o[0] in values and o[1] in slots and slots[o[1]] is None)
            require(re.fullmatch(r'store i64 %' + IDENT + r', ptr %' + IDENT + r', align (?:4|8)', str(i).strip()) is not None)
            slots[o[1]] = values[o[0]]
        elif i.opcode == 'load':
            require(len(o) == 1 and o[0] in slots and slots[o[0]] is not None)
            require(re.fullmatch(r'load i64, ptr %' + IDENT + r', align (?:4|8)', rhs(i)) is not None)
            values[i] = slots[o[0]]
        else:
            raise ValueError('unexpected entry instruction')
    g = list(guard.operands)
    require(len(g) == 2 and values.get(g[0]) == 'n' and g[1].value_kind.name == 'constant_int' and g[1].get_constant_value() == 64)
    require(re.fullmatch(r'icmp ult i64 %' + IDENT + ', 64', rhs(guard)) is not None)
    c = list(branch.operands)
    require(len(c) == 3 and c[0] == guard and c[1] != c[2] and set(c[1:]) == set(blocks[1:]))
    require(re.fullmatch(r'br i1 %' + IDENT + r', label %' + IDENT + r', label %' + IDENT, str(branch).strip()) is not None)
    good, bad = (next(b for b in blocks if b == x) for x in (c[2], c[1]))
    require([str(i).strip() for i in bad.instructions] == ['call void @llvm.trap()', 'unreachable'])
    require(module.get_function('llvm.trap').is_declaration)
    ok = list(good.instructions)
    require(len(ok) == 2 and ok[0].opcode == OPS[operation] and ok[1].opcode == 'ret')
    o = list(ok[0].operands)
    require(len(o) == 2 and values.get(o[0]) == 'a' and values.get(o[1]) == 'n')
    require(re.fullmatch(OPS[operation] + r' i64 %' + IDENT + r', %' + IDENT, rhs(ok[0])) is not None)
    require(list(ok[1].operands) == [ok[0]] and re.fullmatch(r'ret i64 %' + IDENT, str(ok[1]).strip()) is not None)
    return operation


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('ir', type=Path); p.add_argument('symbol'); p.add_argument('operation', choices=OPS)
    a = p.parse_args()
    with a.ir.open('rb') as f: data = f.read(262145)
    validate(data.decode('utf-8'), a.symbol, a.operation)
    print('RESTRICTED_SHIFT_FLOW_VALIDATED', a.operation, a.symbol)
