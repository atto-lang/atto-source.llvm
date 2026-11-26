"""Experimental unoptimized scalar division IR validator; requires llvmlite.
Produces SMT-LIB, not a proof receipt. Only the named function is modeled.
"""
import sys
from pathlib import Path
from llvmlite import binding as llvm


def extract(ir, name):
    if len(ir) > 262144:
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
    require(all('!' not in str(i) for block in blocks for i in block.instructions))
    vals = dict(zip(args, ['a', 'b']))
    slots = {}
    def value(v):
        if v in vals:
            return vals[v]
        require(v.is_constant and str(v.type) == 'i64' and v.value_kind.name == 'constant_int')
        return '(_ bv%d 64)' % v.get_constant_value()
    def evaluate(i):
        op = i.opcode
        o = list(i.operands)
        if op == 'alloca':
            require(str(i).endswith('alloca i64, align 8') and len(o) == 1 and o[0].get_constant_value() == 1)
            slots[i] = None
        elif op == 'store':
            require(str(o[0].type) == 'i64' and o[1] in slots and slots[o[1]] is None and str(i).lstrip().startswith('store i64 ') and str(i).endswith(', align 8'))
            slots[o[1]] = value(o[0])
        elif op == 'load':
            require(str(i.type) == 'i64' and o[0] in slots and slots[o[0]] is not None and 'load i64, ' in str(i) and str(i).endswith(', align 8'))
            vals[i] = slots[o[0]]
        elif op == 'icmp':
            require('icmp eq i64 ' in str(i))
            vals[i] = f'(= {value(o[0])} {value(o[1])})'
        elif op in ('and', 'or'):
            require(str(i.type) == 'i1' and f'{op} i1 ' in str(i))
            vals[i] = f'({op} {value(o[0])} {value(o[1])})'
        elif op == 'select':
            require(str(i.type) == 'i64' and 'select i1 ' in str(i))
            vals[i] = f'(ite {value(o[0])} {value(o[1])} {value(o[2])})'
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
    require('sdiv i64 ' in str(ok[-2]) and value(division[0]) == 'a' and list(ok[-1].operands) == [ok[-2]])
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
    print(extract(Path(sys.argv[1]).read_text(), sys.argv[2]), end='')
