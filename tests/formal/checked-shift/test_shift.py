import unittest
from check_shift import OPS, validate


def representative(op, slots=False):
    prefix = ''
    a, n = '%a', '%n'
    if slots:
        for v in ('a', 'n'):
            prefix += f'  %{v}.slot = alloca i64, align 8\n  store i64 %{v}, ptr %{v}.slot, align 4\n  %{v}.read = load i64, ptr %{v}.slot, align 4\n'
        a, n = '%a.read', '%n.read'
    return f'''declare void @llvm.trap()
define i64 @guest(i64 %a, i64 %n) {{
entry:
{prefix}  %g = icmp ult i64 {n}, 64
  br i1 %g, label %ok, label %trap
trap:
  call void @llvm.trap()
  unreachable
ok:
  %v = {OPS[op]} i64 {a}, {n}
  ret i64 %v
}}
'''


class Shift(unittest.TestCase):
    def test_all_operations(self):
        for op in OPS:
            for slots in (False, True):
                with self.subTest(op=op, slots=slots):
                    self.assertEqual(validate(representative(op, slots), 'guest', op), op)

    def test_reject_mutations(self):
        s = representative('arithmetic-right', True)
        for label, ir in [
            ('wrong signedness', s.replace('ashr i64', 'lshr i64')),
            ('wrong guard', s.replace('icmp ult', 'icmp slt')),
            ('off by one', s.replace(', 64', ', 63')),
            ('wrong guard operand', s.replace('icmp ult i64 %n.read', 'icmp ult i64 %a.read')),
            ('inverted branch', s.replace('label %ok, label %trap', 'label %trap, label %ok')),
            ('no guard', s.replace('br i1 %g', 'br i1 true')),
            ('masked count', s.replace('  %v =', '  %m = and i64 %n.read, 63\n  %v =').replace('ashr i64 %a.read, %n.read', 'ashr i64 %a.read, %m')),
            ('wrong value', s.replace('ashr i64 %a.read, %n.read', 'ashr i64 %n.read, %a.read')),
            ('poison exact', s.replace('ashr i64', 'ashr exact i64')),
            ('wrong result', s.replace('ret i64 %v', 'ret i64 %a')),
            ('extra side effect', s.replace('  %v =', '  store i64 %n, ptr %a.slot, align 4\n  %v =')),
            ('overaligned', s.replace('load i64, ptr %a.slot, align 4', 'load i64, ptr %a.slot, align 16')),
            ('volatile', s.replace('load i64, ptr %a.slot', 'load volatile i64, ptr %a.slot')),
            ('missing trap', s.replace('call void @llvm.trap()\n  unreachable', 'ret i64 0')),
            ('oversized', s + ';' + 'x'*262144),
        ]:
            with self.subTest(label=label):
                self.assertNotEqual(ir, s)
                with self.assertRaises((ValueError, RuntimeError)): validate(ir, 'guest', 'arithmetic-right')


if __name__ == '__main__': unittest.main(verbosity=2)
