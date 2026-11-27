"""Restricted unoptimized terminal i64 remainder/modulo translation validator.
The srem instruction's defined semantics and range are trusted, not re-proved.
"""
import re
import sys
from pathlib import Path
from llvmlite import binding as llvm


def extract(ir, name, operation):
    def require(ok):
        if not ok:
            raise ValueError('unsupported remainder/modulo IR')
    require(operation in ('rem', 'mod') and len(ir.encode('utf-8')) <= 262144)
    module = llvm.parse_assembly(ir)
    module.verify()
    fn = module.get_function(name)
    args, blocks = list(fn.arguments), list(fn.blocks)
    ident = r'[-a-zA-Z$._0-9]+'
    require(str(fn.global_value_type) == 'i64 (i64, i64)' and len(blocks) == 3)
    require(not list(fn.attributes) and all(not list(a.attributes) for a in args))
    require(re.fullmatch(ident, name) is not None)
    require(re.fullmatch(r'define (?:internal )?i64 @' + re.escape(name)
                         + r'\(i64 %' + ident + r', i64 %' + ident + r'\) \{',
                         str(fn).splitlines()[0]) is not None)
    require(all('!' not in str(i) for block in blocks for i in block.instructions))
    def syntax(i):
        text = str(i).strip()
        if text.startswith('%'):
            m = re.fullmatch(r'%' + ident + r' = (.+)', text)
            require(m is not None)
            return m.group(1)
        return text
    vals, slots = dict(zip(args, ['a', 'b'])), {}
    def val(v):
        if v in vals:
            return vals[v]
        require(v.is_constant and str(v.type) == 'i64' and v.value_kind.name == 'constant_int')
        return '(_ bv%d 64)' % v.get_constant_value()
    def expr(op, *parts):
        require(sum(map(len, parts)) + len(op) + len(parts) + 2 <= 16384)
        return '(' + op + ' ' + ' '.join(parts) + ')'
    def evaluate(i, entry):
        op, rhs, o = i.opcode, syntax(i), list(i.operands)
        if entry and op == 'alloca':
            require(rhs == 'alloca i64, align 8' and len(o) == 1 and o[0].get_constant_value() == 1)
            slots[i] = None
        elif entry and op == 'store':
            require(str(o[0].type) == 'i64' and o[1] in slots and slots[o[1]] is None and rhs.startswith('store i64 ') and rhs.endswith((', align 4', ', align 8')))
            slots[o[1]] = val(o[0])
        elif entry and op == 'load':
            require(str(i.type) == 'i64' and o[0] in slots and slots[o[0]] is not None and rhs.startswith('load i64, ') and rhs.endswith((', align 4', ', align 8')))
            vals[i] = slots[o[0]]
        elif op == 'icmp':
            m = re.match(r'icmp (eq|slt|sgt) i64 ', rhs)
            require(m is not None)
            vals[i] = expr({'eq':'=', 'slt':'bvslt', 'sgt':'bvsgt'}[m.group(1)], val(o[0]), val(o[1]))
        elif op in ('and', 'or'):
            require(str(i.type) == 'i1' and rhs.startswith(op + ' i1 '))
            vals[i] = expr(op, val(o[0]), val(o[1]))
        elif op == 'select':
            require(str(i.type) == 'i64' and rhs.startswith('select i1 '))
            vals[i] = expr('ite', *map(val, o))
        elif op in ('add', 'sub') and not entry:
            require(rhs.startswith(op + ' i64 ')) # no poison-producing flags
            vals[i] = expr({'add':'bvadd', 'sub':'bvsub'}[op], *map(val, o))
        else:
            raise ValueError('unsupported instruction: ' + op)
    entry = list(blocks[0].instructions)
    require(1 <= len(entry) <= 32 and entry[-1].opcode == 'br')
    for i in entry[:-1]:
        evaluate(i, True)
    c = list(entry[-1].operands)
    require(len(c) == 3 and str(c[0].type) == 'i1' and c[1] != c[2] and set(c[1:]) == set(blocks[1:]))
    guard = val(c[0])
    bad, good = (next(b for b in blocks if b == x) for x in (c[2], c[1]))
    fail = list(bad.instructions)
    require([i.opcode for i in fail] == ['call', 'unreachable'])
    call = list(fail[0].operands)
    require(len(call) == 1 and call[0].name == 'llvm.trap' and str(fail[0]).strip() == 'call void @llvm.trap()')
    require(module.get_function('llvm.trap').is_declaration)
    ok = list(good.instructions)
    require(2 <= len(ok) <= 24 and ok[-1].opcode == 'ret')
    divisor, remainder = None, None
    for i in ok[:-1]:
        if i.opcode == 'srem':
            o = list(i.operands)
            require(divisor is None and syntax(i).startswith('srem i64 ') and val(o[0]) == 'a')
            divisor, remainder = val(o[1]), i
            vals[i] = 'r'
        else:
            evaluate(i, False)
    require(divisor is not None)
    result = list(ok[-1].operands)[0]
    if operation == 'rem':
        require(result == remainder)
    output = val(result)
    # The guard and operand-substitution obligations do not depend on abstract r.
    # Require divisor expression to be independent of the result, by construction.
    text = f'''(set-logic QF_BV)
(set-option :timeout 10000)
(declare-const a (_ BitVec 64))
(declare-const b (_ BitVec 64))
(declare-const r (_ BitVec 64))
(define-fun z () (_ BitVec 64) (_ bv0 64))
(define-fun one () (_ BitVec 64) (_ bv1 64))
(define-fun negone () (_ BitVec 64) (_ bv18446744073709551615 64))
(define-fun guard () Bool {guard})
(define-fun divisor () (_ BitVec 64) {divisor})
(define-fun output () (_ BitVec 64) {output})
(push) (assert (not (= guard (= b z)))) (check-sat) (pop)
(push) (assert (not guard)) (assert (or (= divisor z) (and (= a (_ bv9223372036854775808 64)) (= divisor negone)))) (check-sat) (pop)
(push) (assert (not guard)) (assert (not (= b negone))) (assert (not (= divisor b))) (check-sat) (pop)
(push) (assert (not guard)) (assert (= b negone)) (assert (not (= divisor one))) (check-sat) (pop)
'''
    if operation == 'mod':
        text += '''(define-fun bw () (_ BitVec 65) ((_ sign_extend 1) b))
(define-fun rw () (_ BitVec 65) ((_ sign_extend 1) r))
(define-fun zw () (_ BitVec 65) (_ bv0 65))
(define-fun mag () (_ BitVec 65) (ite (bvslt bw zw) (bvneg bw) bw))
(define-fun reference () (_ BitVec 65) (ite (bvslt rw zw) (bvadd rw mag) rw))
(define-fun admitted () Bool (and (not (= b z)) (bvslt rw mag) (bvsgt rw (bvneg mag))))
(push) (assert admitted) (assert (not (= ((_ sign_extend 1) output) reference))) (check-sat) (pop)
(push) (assert admitted) (assert (not (and (bvsge ((_ sign_extend 1) output) zw) (bvslt ((_ sign_extend 1) output) mag)))) (check-sat) (pop)
'''
    return text + '''(push) (assert guard) (check-sat) (pop)
(push) (assert (not guard)) (assert (= b negone)) (check-sat) (pop)
(push) (assert (not guard)) (assert (not (= b negone))) (check-sat) (pop)
'''

if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('usage: validate_scalar_remainder_ir.py input.ll symbol rem|mod')
    with Path(sys.argv[1]).open('rb') as f:
        data = f.read(262145)
    print(extract(data.decode('utf-8'), sys.argv[2], sys.argv[3]), end='')
