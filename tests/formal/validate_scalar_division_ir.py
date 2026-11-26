"""Experimental unoptimized scalar division IR validator; requires llvmlite.
Produces SMT-LIB, not a proof receipt. Only the named function is modeled.
"""
import re
import sys
from pathlib import Path
from llvmlite import binding as llvm


def extract(ir, name):
    if len(ir.encode('utf-8')) > 262144:
        raise ValueError('IR exceeds verifier input budget')
    module = llvm.parse_assembly(ir)
    module.verify()
    fn = module.get_function(name)
    args = list(fn.arguments)
    blocks = list(fn.blocks)
    def require(condition):
        if not condition:
            raise ValueError('unsupported scalar division IR')
    require(str(fn.global_value_type) == 'i64 (i64, i64)' and len(blocks) == 3)
    require(not list(fn.attributes) and all(not list(a.attributes) for a in args))
    # Only the plain C-convention scalar function header is in scope.
    ident = r'[-a-zA-Z$._0-9]+'
    require(re.fullmatch(ident, name) is not None)
    require(re.fullmatch(r'define (?:internal )?i64 @' + re.escape(name)
                         + r'\(i64 %' + ident + r', i64 %' + ident + r'\) \{',
                         str(fn).splitlines()[0]) is not None)
    def syntax(i):
        # Inspect the opcode RHS, never search arbitrary SSA identifiers.
        text = str(i).strip()
        if text.startswith('%'):
            match = re.fullmatch(r'%' + ident + r' = (.+)', text)
            require(match is not None)
            return match.group(1)
        return text
    require(all('!' not in str(i) for block in blocks for i in block.instructions))
    vals = dict(zip(args, ['a', 'b']))
    slots = {}
    def value(v):
        if v in vals:
            return vals[v]
        require(v.is_constant and str(v.type) == 'i64' and v.value_kind.name == 'constant_int')
        return '(_ bv%d 64)' % v.get_constant_value()
    def expression(operator, *parts):
        # Bound intermediate expansion before concatenation, even for shared SSA DAGs.
        require(sum(map(len, parts)) + len(operator) + len(parts) + 2 <= 16384)
        return '(' + operator + ' ' + ' '.join(parts) + ')'
    def evaluate(i):
        op = i.opcode
        rhs = syntax(i)
        o = list(i.operands)
        if op == 'alloca':
            require(rhs == 'alloca i64, align 8' and len(o) == 1 and o[0].get_constant_value() == 1)
            slots[i] = None
        elif op == 'store':
            require(str(o[0].type) == 'i64' and o[1] in slots and slots[o[1]] is None and rhs.startswith('store i64 ') and rhs.endswith((', align 4', ', align 8')))
            slots[o[1]] = value(o[0])
        elif op == 'load':
            require(str(i.type) == 'i64' and o[0] in slots and slots[o[0]] is not None and rhs.startswith('load i64, ') and rhs.endswith((', align 4', ', align 8')))
            vals[i] = slots[o[0]]
        elif op == 'icmp':
            require(rhs.startswith('icmp eq i64 '))
            vals[i] = expression('=', value(o[0]), value(o[1]))
        elif op in ('and', 'or'):
            require(str(i.type) == 'i1' and rhs.startswith(f'{op} i1 '))
            vals[i] = expression(op, value(o[0]), value(o[1]))
        elif op == 'select':
            require(str(i.type) == 'i64' and rhs.startswith('select i1 '))
            vals[i] = expression('ite', value(o[0]), value(o[1]), value(o[2]))
        else:
            raise ValueError('unsupported instruction: ' + op)
    entry = list(blocks[0].instructions)
    require(len(entry) <= 32 and entry[-1].opcode == 'br')
    for i in entry[:-1]:
        evaluate(i)
    cond = list(entry[-1].operands)
    require(len(cond) == 3 and str(cond[0].type) == 'i1')
    guard = value(cond[0])
    # LLVM's C API lists conditional branch operands as condition, false, true.
    require(cond[1] != cond[2] and set(cond[1:]) == set(blocks[1:]))
    bad = next(b for b in blocks if b == cond[2])
    good = next(b for b in blocks if b == cond[1])
    fail = list(bad.instructions)
    require([i.opcode for i in fail] == ['call', 'unreachable'])
    call = list(fail[0].operands)
    require(len(call) == 1 and call[0].name == 'llvm.trap' and str(fail[0]).strip() == 'call void @llvm.trap()')
    ok = list(good.instructions)
    require(2 <= len(ok) <= 16 and [i.opcode for i in ok[-2:]] == ['sdiv', 'ret'])
    for i in ok[:-2]:
        require(i.opcode in ('icmp', 'and', 'or', 'select'))
        evaluate(i)
    division = list(ok[-2].operands)
    require(syntax(ok[-2]).startswith('sdiv i64 ') and value(division[0]) == 'a' and list(ok[-1].operands) == [ok[-2]])
    divisor = value(division[1])
    return f'''(set-logic QF_BV)
(set-option :timeout 10000)
(declare-const a (_ BitVec 64))
(declare-const b (_ BitVec 64))
(define-fun invalid ((x (_ BitVec 64)) (y (_ BitVec 64))) Bool (or (= y (_ bv0 64)) (and (= x (_ bv9223372036854775808 64)) (= y (_ bv18446744073709551615 64)))))
(define-fun guard () Bool {guard})
(define-fun divisor () (_ BitVec 64) {divisor})
(push) (assert (not (= guard (invalid a b)))) (check-sat) (pop)
(push) (assert (invalid a divisor)) (check-sat) (pop)
(push) (assert (not guard)) (assert (not (= divisor b))) (check-sat) (pop)
(push) (assert guard) (check-sat) (pop)
(push) (assert (not guard)) (check-sat) (pop)
'''

if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: validate.py input.ll exact_function_symbol')
    with Path(sys.argv[1]).open('rb') as source:
        data = source.read(262145)
    if len(data) > 262144:
        raise SystemExit('IR exceeds verifier input budget')
    print(extract(data.decode('utf-8'), sys.argv[2]), end='')
