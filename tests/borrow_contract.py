#!/usr/bin/env python3
"""Shared Java/native borrow contract: reject unsound programs before emitting IR."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


def require(condition, evidence):
    if not condition:
        raise AssertionError(evidence)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--clang', required=True)
    args = parser.parse_args()
    cases = Path(__file__).with_name('fixtures').joinpath('borrow-contract.tsv')
    with tempfile.TemporaryDirectory(prefix='ores-borrow-contract-') as tmp:
        root = Path(tmp)
        for line in cases.read_text().splitlines():
            expected, name, code = line.split('\t')
            source = root / (name + '.ores')
            source.write_text(code)
            result = run([args.compiler, str(source)])
            if expected == 'reject':
                require(result.returncode and not result.stdout, (name, result))
                require('borrow' in result.stderr.lower(), (name, result.stderr))
                require('internal' not in result.stderr.lower(), (name, result.stderr))
            else:
                require(result.returncode == 0, (name, result.stderr))
                ir = root / (name + '.ll')
                ir.write_text(result.stdout)
                binary = root / name
                linked = run([args.clang, '-O1', str(ir), '-o', str(binary)])
                require(linked.returncode == 0, (name, linked.stderr))
                executed = run([str(binary)])
                require(executed.returncode == 42, (name, executed))
    print('PASS shared borrow contract: 18 cases; accepted programs execute native code')


if __name__ == '__main__':
    main()
