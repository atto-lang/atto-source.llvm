#!/usr/bin/env python3
"""Source-only compiler coverage for the actual ores-interfaces REST RouteContract."""
from pathlib import Path
import subprocess
import sys
import tempfile

binary, fixture = sys.argv[1:3]
source=Path(fixture).read_text()
def compiler(text=None, check=True):
    if text is None:
        return subprocess.run([binary,*(['--check'] if check else []),fixture],
            text=True,capture_output=True,timeout=15)
    with tempfile.NamedTemporaryFile(mode='w',suffix='.ores') as fd:
        fd.write(text);fd.flush()
        return subprocess.run([binary,*(['--check'] if check else []),fd.name],
            text=True,capture_output=True,timeout=15)
positive=compiler()
assert positive.returncode==0 and not positive.stdout, positive
rejected=compiler(check=False)
assert rejected.returncode!=0 and 'no admitted LLVM runtime implementation' in rejected.stderr, rejected
bad={
 'duplicate route member':source.replace('end\n','  fnc get(String other) => String;\nend\n'),
 'non-String wire result':source.replace('fnc post(String request) => String;', 'fnc post(String request) => int;'),
 'unsafe method identifier':source.replace('fnc get(', 'fnc await('),
 'invalid parameter list':source.replace('fnc post(String request)', 'fnc post(String request, String request)'),
}
for label, text in bad.items():
    result=compiler(text)
    assert result.returncode!=0 and not result.stdout, (label,result)
print('Canonical ores-interfaces REST RouteContract parsed; negative cases rejected:',len(bad))
