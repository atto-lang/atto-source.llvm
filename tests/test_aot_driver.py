#!/usr/bin/env python3
"""Native output executes without the compiler; rejected source preserves output."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

driver,compiler,clang=map(str,sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='ores aot ') as directory:
    root=Path(directory);source=root/'main.ores';output=root/'native app'
    env=dict(os.environ,ORESLANG_LLVMC=compiler,CLANG=clang)
    source.write_text('fnc main(): int { return 42; }\n')
    subprocess.run([driver,str(source),str(output)],env=env,check=True)
    assert subprocess.run([str(output)],env={'PATH':'/nonexistent'}).returncode==42
    original=output.read_bytes()
    for text in ['import module spin from "spin";\n', 'this is not Oreslang\n', 'fnc helper(): int { return 42; }\n']:
        source.write_text(text)
        result=subprocess.run([driver,str(source),str(output)],env=env,capture_output=True)
        assert result.returncode!=0
        assert output.read_bytes()==original
    assert not list(root.glob('.ores-aot.*'))
print('native AOT driver: PASS')
