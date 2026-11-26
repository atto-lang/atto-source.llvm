import argparse
import subprocess
from pathlib import Path
from validate_scalar_remainder_ir import extract
if not __debug__:
    raise RuntimeError('regression assertions must be enabled')
parser = argparse.ArgumentParser()
parser.add_argument('--solver', default='z3')
parser.add_argument('--compiler')
parser.add_argument('--rem-symbol', default='remainder')
parser.add_argument('--mod-symbol', default='modulo')
args = parser.parse_args()

def run(argv, data=None):
    p = subprocess.run(argv, input=data, capture_output=True, text=True, timeout=60)
    if p.returncode or p.stderr or len(p.stdout.encode()) > 262144:
        raise RuntimeError(f'failed or excessive output: {argv[0]}')
    return p.stdout

version = run([args.solver, '-version']).strip().split()
if version[:3] not in (['Z3','version','4.13.3'], ['Z3','version','4.13.3.0']):
    raise RuntimeError('expected Z3 4.13.3')

def solve(smt):
    return run([args.solver, '-in', '-smt2'], smt).splitlines()
base=Path(__file__).with_name('representative_rem_mod.ll').read_text()
rem=base[:base.index('  %neg =')]+'  ret i64 %rem\n}\n'
for name,ir,op,count in [('mod',base,'mod',6),('rem',rem,'rem',4)]:
    smt=extract(ir,'modulo',op);result=solve(smt);assert result==['unsat']*count+['sat']*3,result
    print(name, result)
for label,ir in [('wrong zero guard',base.replace('icmp eq i64 %y, 0','icmp eq i64 %y, 1')),('unsafe -1',base.replace('i64 1, i64 %y','i64 -1, i64 %y')),('wrong safe divisor',base.replace('i64 1, i64 %y','i64 1, i64 1')),('wrong correction',base.replace('i64 %add, i64 %sub','i64 %add, i64 %add')),('wrong result',base.replace('ret i64 %result','ret i64 %rem'))]:
    result=solve(extract(ir,'modulo','mod'));assert len(result)==9 and all(x in ('sat','unsat') for x in result) and 'sat' in result[:6] and result[6:]==['sat']*3,result;print('MUTATION DETECTED',label,result)
for label,ir in [('unsigned',base.replace('srem i64','urem i64')),('wrong numerator',base.replace('srem i64 %x','srem i64 %y')),('poison flag',base.replace('add i64 %rem','add nsw i64 %rem')),('overalign',base.replace('load i64, ptr %a.slot, align 4','load i64, ptr %a.slot, align 16')),('volatile',base.replace('load i64, ptr %a.slot','load volatile i64, ptr %a.slot')),('quoted name',base.replace('%rem = srem','%"quoted rem" = srem').replace('%rem,','%"quoted rem",').replace('%rem\n','%"quoted rem"\n')),('wrong branch',base.replace('label %trap, label %ok','label %ok, label %trap'))]:
    try: extract(ir,'modulo','mod')
    except (ValueError,RuntimeError): print('REJECTED',label)
    else: raise AssertionError(label)
print('REPRESENTATIVE_REM_MOD_REGRESSION 14/14')
if args.compiler:
    fixture = Path(__file__).resolve().parent.parent / 'fixtures' / 'formal-dynamic-rem-mod.atto'
    ir = run([args.compiler, str(fixture)])
    for operation, symbol, n in [('rem', args.rem_symbol, 4), ('mod', args.mod_symbol, 6)]:
        if solve(extract(ir, symbol, operation)) != ['unsat'] * n + ['sat'] * 3:
            raise RuntimeError(f'compiler-emitted {operation} proof failed')
    print('COMPILER_EMITTED_REM_MOD_IR_PROVED (restricted named functions only)')
else:
    print('COMPILER_EMITTED_REM_MOD_IR_NOT_EXECUTED')
