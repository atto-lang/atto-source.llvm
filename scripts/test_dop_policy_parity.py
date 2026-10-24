#!/usr/bin/env python3
"""Check each real backend policy against a shared independent logical-place oracle.

Use both roots for differential testing, or one root for a repository-local CI check.
Requires Python 3, g++17 for LLVM, and Java 17+ with jdk.compiler for Java.
"""
import argparse
import itertools
from pathlib import Path
import random
import subprocess
import tempfile

# Character.isWhitespace, deliberately excluding NBSP, figure space and narrow NBSP.
BLANK = ''.join(map(chr, [9, 10, 11, 12, 13, 28, 29, 30, 31, 32, 0x1680,
    *range(0x2000, 0x2007), *range(0x2008, 0x200b), 0x2028, 0x2029, 0x205f, 0x3000]))


def invalid(p):
    layout, store, arch, comp, field, begin, end, epoch, access = p
    return (layout not in range(3) or access not in range(2)
            or any(s is not None and (not s or all(c in BLANK for c in s))
                   for s in (store, arch, comp, field))
            or (comp is None and field is not None)
            or (begin is not None and begin >= end))


def oracle(a, b):
    if invalid(a) or invalid(b):
        return 'invalid'
    if a[1] != b[1]:
        return 'independent'
    if a[7] != b[7]:
        return 'invalid'
    if any(a[i] is not None and b[i] is not None and a[i] != b[i] for i in (2, 3, 4)):
        return 'independent'
    overlap = a[5] is None or b[5] is None or a[5] < b[6] and b[5] < a[6]
    return 'conflict' if overlap and (a[8] == 1 or b[8] == 1) else 'independent'


def cases():
    rng = random.Random(0x0AE5)
    base = [0, 'world', 'A', 'Position', 'x', 0, 8, 0, 0]
    descriptors = []
    for layout, access, store, arch, comp, rows, epoch in itertools.product(
            range(3), range(2), ('world', 'other'), (None, 'A', 'B'),
            ((None, None), ('Position', None), ('Position', 'x'), ('Position', 'y'), ('Velocity', 'x')),
            ((None, None), (0, 4), (4, 8), (2, 6)), (0, 1)):
        descriptors.append([layout, store, arch, *comp, *rows, epoch, access])
    pairs = [(base, p) for p in descriptors]
    pairs += [(rng.choice(descriptors), rng.choice(descriptors)) for _ in range(12000)]
    # Check every Java whitespace code point individually in all identity positions;
    # UTF-8 encodings and non-breaking-space exceptions must agree.
    for identity in ('', *BLANK, BLANK, '\u00a0', '\u2007', '\u202f', ' x '):
        for position in (1, 2, 3, 4):
            p = base.copy()
            p[position] = identity
            pairs.append((p, base))
    for position, value in ((0, 99), (8, 99), (6, 0), (3, None)):
        p = base.copy()
        p[position] = value
        pairs.append((p, base))
    # Shared nonnegative signed-long boundary. Native u64 is a permitted extension.
    for begin, end in ((2**63-2, 2**63-1), (0, 2**63-1)):
        p = base.copy()
        p[5:8] = [begin, end, 2**63-1]
        pairs.append((p, p))
    return pairs + [(b, a) for a, b in pairs]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-root', type=Path)
    parser.add_argument('--llvm-root', type=Path)
    args = parser.parse_args()
    if not (args.java_root or args.llvm_root):
        parser.error('pass --java-root and/or --llvm-root')
    pairs = cases()
    data = ''.join('\t'.join('-' if x is None else x.encode('utf-8').hex() if isinstance(x, str) else str(x) for x in a+b)+'\n' for a,b in pairs)
    expected = [oracle(a,b) for a,b in pairs]
    with tempfile.TemporaryDirectory(prefix='ores-dop-parity-') as tmp:
        commands = []
        if args.java_root:
            root = args.java_root.resolve()
            subprocess.run(['java', '-m', 'jdk.compiler/com.sun.tools.javac.Main', '-d', tmp,
                str(root/'src/main/java/dev/oreslang/types/DataOrientedBorrowChecker.java'),
                str(root/'verification/DopPolicyProbe.java')], check=True)
            commands.append(('Java', ['java', '-cp', tmp, 'DopPolicyProbe']))
        if args.llvm_root:
            root = args.llvm_root.resolve()
            binary = str(Path(tmp)/'dop-policy')
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I'+str(root/'src/main/cpp'), str(root/'verification/dop_policy_probe.cpp'), '-o', binary], check=True)
            commands.append(('LLVM C++ policy', [binary]))
        for name, command in commands:
            actual = subprocess.run(command, input=data, text=True, encoding='utf-8',
                                    capture_output=True, check=True).stdout.splitlines()
            if len(actual) != len(expected):
                raise AssertionError(f'{name}: {len(actual)} results, expected {len(expected)}')
            for i, (got, want) in enumerate(zip(actual, expected)):
                if got != want:
                    raise AssertionError(f'{name}: case {i}: {pairs[i]!r}: got {got}, expected {want}')
            print(f'{name}: {len(expected)} bidirectional cases match independent oracle; malformed-set regressions passed')


if __name__ == '__main__':
    main()
