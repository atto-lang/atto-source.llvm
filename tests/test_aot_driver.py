#!/usr/bin/env python3
"""Native output executes without the compiler; rejected source preserves output.

The full/thin LTO cases invoke the REAL native Oreslang compiler and link
with Clang+LLD when LLD is installed. Missing LLD is explicitly skipped:
the basic non-LTO path remains mandatory.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

driver,compiler,clang=map(str,sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='ores aot ') as directory:
    root=Path(directory);source=root/'main.ores';output=root/'native app'
    env=dict(os.environ,ORESLANG_LLVMC=compiler,CLANG=clang,ORESLANG_LTO='off')
    valid='fnc main(): int { return 42; }\n'
    source.write_text(valid)
    subprocess.run([driver,str(source),str(output)],env=env,check=True)
    assert subprocess.run([str(output)],env={'PATH':'/nonexistent'}).returncode==42
    original=output.read_bytes()
    for text in ['import module spin from "spin";\n', 'this is not Oreslang\n',
                 'fnc helper(): int { return 42; }\n']:
        source.write_text(text)
        result=subprocess.run([driver,str(source),str(output)],env=env,capture_output=True)
        assert result.returncode!=0
        assert output.read_bytes()==original

    # Invalid LTO modes must never fall back to baseline compilation.
    source.write_text(valid)
    bad=dict(env,ORESLANG_LTO='unknown')
    result=subprocess.run([driver,str(source),str(output)],env=bad,capture_output=True)
    assert result.returncode==64
    assert output.read_bytes()==original

    # Each requested mode must really build, link and run the native guest
    # rather than pass a mock IR through a linker. A missing LLD is explicit.
    if shutil.which('ld.lld') is None:
        print('native AOT LTO full/thin: SKIP (ld.lld not installed)')
    else:
        for mode in ('full','thin'):
            lto_env=dict(env,ORESLANG_LTO=mode)
            subprocess.run([driver,str(source),str(output)],env=lto_env,check=True)
            assert subprocess.run([str(output)],env={'PATH':'/nonexistent'}).returncode==42
            # Both source-admission and publisher atomicity still apply in LTO mode.
            previous=output.read_bytes()
            source.write_text('this is not Oreslang\n')
            rejected=subprocess.run([driver,str(source),str(output)],
                env=lto_env,capture_output=True)
            assert rejected.returncode!=0
            assert output.read_bytes()==previous
            source.write_text(valid)
    assert not list(root.glob('.ores-aot.*'))
print('native AOT driver: PASS')
