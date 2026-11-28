"""Executable positive and mutation checks for the restricted validator."""
import unittest
from check_intrinsic import validate


def representative(op, slots=False):
    intrinsic='llvm.'+{'add':'sadd','sub':'ssub','mul':'smul','neg':'ssub'}[op]+'.with.overflow.i64'
    args='i64 %a' if op=='neg' else 'i64 %a, i64 %b'
    prefix=''
    x,y='%a','%b'
    if slots:
        for v in ('a',) if op=='neg' else ('a','b'):
            prefix+=f'  %{v}.slot = alloca i64, align 8\n  store i64 %{v}, ptr %{v}.slot, align 4\n  %{v}.read = load i64, ptr %{v}.slot, align 4\n'
        x,y='%a.read','%b.read'
    if op=='neg': x,y='0',x
    return f'''declare {{i64,i1}} @{intrinsic}(i64,i64)
declare void @llvm.trap()
define i64 @guest({args}) {{
entry:
{prefix}  %p = call {{i64,i1}} @{intrinsic}(i64 {x}, i64 {y})
  %v = extractvalue {{i64,i1}} %p, 0
  %o = extractvalue {{i64,i1}} %p, 1
  br i1 %o, label %trap, label %ok
trap:
  call void @llvm.trap()
  unreachable
ok:
  ret i64 %v
}}
'''


class Contract(unittest.TestCase):
    def test_all_operations(self):
        for op in ('add','sub','mul','neg'):
            for slots in (False,True):
                with self.subTest(op=op,slots=slots): self.assertEqual(validate(representative(op,slots),'guest',op),op)

    def test_mutants(self):
        s=representative('mul',True)
        for label,ir in [
            ('wrong op',s.replace('smul','sadd')),
            ('unsigned',s.replace('smul','umul')),
            ('wrong operand',s.replace('i64 %b.read)','i64 %a.read)')),
            ('inverted branch',s.replace('label %trap, label %ok','label %ok, label %trap')),
            ('no guard',s.replace('br i1 %o','br i1 false')),
            ('wrong return',s.replace('ret i64 %v','ret i64 %a')),
            ('wrong slot',s.replace('store i64 %b, ptr %b.slot','store i64 %a, ptr %b.slot')),
            ('overalign',s.replace('load i64, ptr %a.slot, align 4','load i64, ptr %a.slot, align 16')),
            ('volatile',s.replace('load i64, ptr %a.slot','load volatile i64, ptr %a.slot')),
            ('extra store',s.replace('  %p =','  store i64 %b, ptr %a.slot, align 4\n  %p =')),
            ('wrong trap',s.replace('call void @llvm.trap()','call void @other()')+'declare void @other()\n'),
            ('quoted SSA',s.replace('%p =','%"quoted pair" =').replace('%p,','%"quoted pair",')),
            ('call attr',s.replace('i64 %b.read)','i64 %b.read) nounwind')),
            ('ret poison',s.replace('ret i64 %v','ret i64 poison')),
            ('oversize',s+';'+'x'*262144),
        ]:
            with self.subTest(label=label):
                self.assertNotEqual(ir,s)
                with self.assertRaises((ValueError,RuntimeError)): validate(ir,'guest','mul')
        with self.assertRaises((ValueError,RuntimeError)):
            validate(representative('neg').replace('i64 0, i64 %a','i64 1, i64 %a'),'guest','neg')

if __name__=='__main__': unittest.main(verbosity=2)
