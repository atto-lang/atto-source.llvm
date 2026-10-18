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
    # The IR fence is a release *screen*, not linked-image certification.
    source.write_text('fnc main(): int { return 42; }\\n')
    fake=root/'fake-ir-compiler.sh'
    for invalid_ir in [
        'define void @guest() gc "shadow-stack" { ret void }',
        'declare void @llvm.gcroot(ptr, ptr)',
        'declare token @llvm.gc.statepoint.p0(i64, i32, ptr, i32, i32, ...)',
        'declare ptr @GC_malloc(i64)',
        'declare void @gc_collect()',
    ]:
        fake.write_text("#!/bin/sh\\ncat <<'IR'\\n" + invalid_ir + "\\nIR\\n")
        fake.chmod(0o755)
        bad_env=dict(env,ORESLANG_LLVMC=str(fake))
        bad=subprocess.run([driver,str(source),str(output)],env=bad_env,capture_output=True)
        assert bad.returncode!=0, invalid_ir
        assert b'STRICT_NO_GC_IR_REJECTED' in bad.stderr, bad.stderr
        assert output.read_bytes()==original
    assert not list(root.glob('.ores-aot.*'))
print('native AOT driver: PASS')
