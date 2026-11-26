"""Replay the restricted division proof and mutations with a trusted Z3 CLI.
Optional --compiler consumes actual compiler output; no representative fallback.
"""
import argparse
from pathlib import Path
import subprocess

from validate_scalar_division_ir import extract

ROOT = Path(__file__).resolve().parent
EXPECTED = ['unsat'] * 3 + ['sat'] * 2


def execute(argv, data=None):
    p = subprocess.run(argv, input=data, capture_output=True, text=True, timeout=60)
    if p.returncode or p.stderr or len(p.stdout.encode()) > 262144:
        raise RuntimeError(f'command failed or exceeded output budget: {argv[0]}')
    return p.stdout


def solve(solver, ir, symbol):
    return execute([solver, '-in', '-smt2'], extract(ir, symbol)).splitlines()


def regress(solver):
    base = (ROOT / 'representative_division.ll').read_text()
    cases = [
        ('correct', base, EXPECTED),
        ('missing overflow', base.replace('%z, %o', '%z, %z'), ['sat', 'sat', 'unsat', 'sat', 'sat']),
        ('wrong safe value', base.replace('i64 1, i64 %y', 'i64 0, i64 %y'), ['unsat', 'sat', 'unsat', 'sat', 'sat']),
        ('wrong admitted divisor', base.replace('i64 1, i64 %y', 'i64 1, i64 1'), ['unsat', 'unsat', 'sat', 'sat', 'sat']),
        ('reversed branch', base.replace('label %trap, label %ok', 'label %ok, label %trap'), None),
        ('unsigned', base.replace('sdiv', 'udiv'), None),
        ('exact flag', base.replace('sdiv i64', 'sdiv exact i64'), None),
        ('wrong numerator', base.replace('sdiv i64 %x', 'sdiv i64 %y'), None),
        ('wrong return', base.replace('ret i64 %q', 'ret i64 %x'), None),
        ('wrong alignment', base.replace('load i64, ptr %sa, align 8', 'load i64, ptr %sa, align 16'), None),
        ('volatile load', base.replace('load i64, ptr %sa', 'load volatile i64, ptr %sa'), None),
        ('quoted predicate', base.replace('%z', '%"icmp eq i64 hidden"').replace('icmp eq i64 %y, 0', 'icmp ne i64 %y, 0'), None),
        ('quoted division', base.replace('%q', '%"sdiv i64 hidden"').replace('sdiv i64 %x, %s', 'sdiv exact i64 %x, %s'), None),
        ('alternate convention', base.replace('define i64', 'define fastcc i64'), None),
        ('return attribute', base.replace('define i64','define noundef i64'), None),
        ('unnamed args', base.replace('%a', '%0').replace('%b', '%1'), EXPECTED),
    ]
    expanded = base.replace('  br i1 %g,', ''.join(
        f'  %wide{i} = or i1 %{prev}, %{prev}\n'
        for i in range(19) for prev in ['g' if i == 0 else f'wide{i-1}']) + '  br i1 %wide18,')
    cases.extend([('expression budget', expanded, None), ('input budget', base + ';' + 'x'*262144, None)])
    for label, ir, expected in cases:
        # Only extractor rejection counts as a structural negative; solver failure never does.
        try:
            smt = extract(ir, 'quotient')
        except (ValueError, RuntimeError):
            if expected is not None:
                raise
        else:
            if expected is None or execute([solver, '-in', '-smt2'], smt).splitlines() != expected:
                raise RuntimeError(f'wrong verdict: {label}')
        print('PASS', label)
    print(f'REPRESENTATIVE_IR_REGRESSION {len(cases)}/{len(cases)}')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--solver', default='z3')
    p.add_argument('--compiler')
    p.add_argument('--symbol', default='quotient')
    args = p.parse_args()
    version = execute([args.solver, '-version']).strip()
    if version.split()[:3] not in (['Z3', 'version', '4.13.3'], ['Z3', 'version', '4.13.3.0']):
        raise RuntimeError('expected Z3 4.13.3')
    regress(args.solver)
    if args.compiler:
        ir = execute([args.compiler, str(ROOT.parent / 'fixtures' / 'formal-dynamic-division.atto')])
        if solve(args.solver, ir, args.symbol) != EXPECTED:
            raise RuntimeError('compiler-generated IR proof failed')
        print('COMPILER_EMITTED_IR_GUARD_PROVED (restricted named function only)')
    else:
        print('COMPILER_EMITTED_IR_NOT_EXECUTED')


if __name__ == '__main__':
    main()
