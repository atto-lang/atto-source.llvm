import unittest
from check_shift import validate


def representative(op,stack=False):
    x,n='%a','%n'
    prefix=''
    if stack:
        for v in ('a','n'):
            prefix+=f'  %{v}.slot = alloca i64, align 8\n  store i64 %{v}, ptr %{v}.slot, align 4\n  %{v}.read = load i64, ptr %{v}.slot, align 4\n'
        x,n='%a.read','%n.read'
    return f'''declare void @llvm.trap()
define i64 @guest(i64 %a, i64 %n) {{
entry:
{prefix}  %guard = icmp ult i64 {n}, 64
  br i1 %guard, label %ok, label %bad
bad:
  call void @llvm.trap()
  unreachable
ok:
  %value = {op} i64 {x}, {n}
  ret i64 %value
}}
'''


class Contract(unittest.TestCase):
    def test_valid(self):
        for op in ('shl','lshr','ashr'):
            for stack in (False,True):
                with self.subTest(op=op,stack=stack): self.assertEqual(validate(representative(op,stack),'guest',op),op)
        print('6 representative accepted shapes')

    def test_reject(self):
        s=representative('ashr',True)
        mutations=[
            ('signed guard',s.replace('icmp ult','icmp slt')),
            ('off-by-one',s.replace(', 64',', 63')),
            ('wrong guard operand',s.replace('icmp ult i64 %n.read','icmp ult i64 %a.read')),
            ('inverted guard',s.replace('label %ok, label %bad','label %bad, label %ok')),
            ('wrong opcode',s.replace('ashr i64','lshr i64')),
            ('exact poison',s.replace('ashr i64','ashr exact i64')),
            ('wrong value',s.replace('ashr i64 %a.read','ashr i64 %n.read')),
            ('wrong count',s.replace('ashr i64 %a.read, %n.read','ashr i64 %a.read, %a.read')),
            ('masked count',s.replace('  %value =','  %masked = and i64 %n.read, 63\n  %value =').replace('ashr i64 %a.read, %n.read','ashr i64 %a.read, %masked')),
            ('wrong return',s.replace('ret i64 %value','ret i64 %a')),
            ('wrong failure',s.replace('call void @llvm.trap()','call void @other()')+'declare void @other()\n'),
            ('overalignment',s.replace('load i64, ptr %a.slot, align 4','load i64, ptr %a.slot, align 16')),
            ('wrong slot',s.replace('store i64 %n, ptr %n.slot','store i64 %a, ptr %n.slot')),
            ('volatile load',s.replace('load i64, ptr %a.slot','load volatile i64, ptr %a.slot')),
            ('oversized',s+';'+'x'*262144),
            ('wrong branch condition',s.replace('br i1 %guard','br i1 true')),
        ]
        for label,ir in mutations:
            with self.subTest(label=label):
                self.assertNotEqual(ir,s)
                with self.assertRaises((ValueError,RuntimeError)): validate(ir,'guest','ashr')
        print(f'{len(mutations)} rejected mutations')

if __name__=='__main__':unittest.main(verbosity=2)
