#!/usr/bin/env python3
"""Native LLVM parser admission: checked declarations only, never codegen."""
import pathlib
import subprocess
import sys
import tempfile

compiler, fixture = sys.argv[1:]
source = pathlib.Path(fixture).read_text()

def call(args, text=None):
    if text is None:
        return subprocess.run([compiler, *args, fixture], text=True, capture_output=True, timeout=12)
    with tempfile.NamedTemporaryFile(mode='w', suffix='.ores') as f:
        f.write(text)
        f.flush()
        return subprocess.run([compiler, *args, f.name], text=True, capture_output=True, timeout=12)

native = call(['--check'])
assert native.returncode == 0 and not native.stdout and not native.stderr, native
rejected = call([])
assert rejected.returncode != 0 and 'no admitted LLVM runtime implementation' in rejected.stderr, rejected
assert not rejected.stdout, rejected.stdout
cases = {
    'duplicate method': source.replace('end\n','  fnc list_items(String x) => String;\nend\n'),
    'duplicate parameter': source.replace('String page_size)', 'String page_size, String page_size)'),
    'reserved operation': source.replace('fnc list_items', 'fnc suspends'),
    'unsupported wire type': source.replace('String page_size', 'int page_size'),
    'missing result type': source.replace('=> String;', '=> ;'),
    'unterminated contract': source.rsplit('end', 1)[0],
}
for label, invalid in cases.items():
    failure = call(['--check'], invalid)
    assert failure.returncode != 0, (label, failure)
    assert not failure.stdout, (label, failure.stdout)
print('Native LLVM generated String client declaration parsed, negative cases rejected, and runtime codegen denied:',len(cases))
