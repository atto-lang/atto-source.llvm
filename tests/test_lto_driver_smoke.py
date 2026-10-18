#!/usr/bin/env python3
"""Source-free AOT driver/linker gate, NOT an Oreslang compiler test.

Uses an explicitly synthetic IR emitter to check the real Clang/LLD invocation,
mode admission and atomic output preservation. Source-level Oreslang tests are
separate and mandatory (see test_aot_driver.py).
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

def require(ok, detail):
    if not ok:
        raise RuntimeError(f'FAILED: {detail}')

driver, clang = map(str, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="ores lto smoke ") as tmp:
    root=Path(tmp); source=root/"guest input.ores"; output=root/"guest binary"
    emitter=root/"fake-llvmc.py"
    emitter.write_text('''#!/usr/bin/env python3
import pathlib,sys
if len(sys.argv)!=3 or sys.argv[1]!='--target=host':sys.exit(64)
data=pathlib.Path(sys.argv[2]).read_text()
if data.strip()!='good':print('not valid LLVM IR');sys.exit(0)
print('define i32 @main() { ret i32 42 }')
''')
    emitter.chmod(0o700)
    source.write_text("good\n")
    env=dict(os.environ, ORESLANG_LLVMC=str(emitter), CLANG=clang)
    modes=['off']+(['full','thin'] if shutil.which('ld.lld') else [])
    for mode in modes:
        current=dict(env, ORESLANG_LTO=mode)
        done=subprocess.run([driver,str(source),str(output)],env=current,
            capture_output=True,text=True)
        require(done.returncode==0, (mode,done.stderr,done.stdout))
        require(subprocess.run([str(output)],env={'PATH':'/nonexistent'}).returncode==42, "smoke assertion failed: subprocess.run([str(output)],env={'PATH':'/nonexistent'}).returncode==42")
        good_bytes=output.read_bytes()
        # Unsupported guest code may never overwrite the published executable.
        source.write_text('not-admitted\n')
        bad=subprocess.run([driver,str(source),str(output)],env=current,
            capture_output=True,text=True)
        require(bad.returncode!=0, (mode,bad.stderr))
        require(output.read_bytes()==good_bytes, mode)
        source.write_text("good\n")
    invalid=subprocess.run([driver,str(source),str(output)],
        env=dict(env,ORESLANG_LTO='unknown'),capture_output=True,text=True)
    require(invalid.returncode==64, 'smoke assertion failed: invalid.returncode==64')
    # A source cannot be the destination (even with a space in its name).
    invalid=subprocess.run([driver,str(source),str(source)],
        env=dict(env,ORESLANG_LTO='off'),capture_output=True,text=True)
    require(invalid.returncode==64, 'smoke assertion failed: invalid.returncode==64')
    require(source.read_text()=="good\n", 'smoke assertion failed: source.read_text()=="good\\n"')
    # The source and output may have distinct names but the same inode.
    # Reject before invoking the compiler or changing either path.
    aliased_output=root/"alias output binary"
    aliased_output.symlink_to(source)
    denied=subprocess.run([driver,str(source),str(aliased_output)],env=env,
        capture_output=True,text=True)
    require(denied.returncode==64, (denied.stdout,denied.stderr))
    require(aliased_output.is_symlink() and source.read_text()=="good\n", 'smoke assertion failed: aliased_output.is_symlink() and source.read_text()=="good\\n"')
    hardlinked_output=root/"hardlinked output binary"
    os.link(source,hardlinked_output)
    denied=subprocess.run([driver,str(source),str(hardlinked_output)],env=env,
        capture_output=True,text=True)
    require(denied.returncode==64, (denied.stdout,denied.stderr))
    require(source.read_text()=="good\n", 'smoke assertion failed: source.read_text()=="good\\n"')
    input_alias=root/"input alias to executable.ores"
    input_alias.symlink_to(output)
    original_binary=output.read_bytes()
    denied=subprocess.run([driver,str(input_alias),str(output)],env=env,
        capture_output=True,text=True)
    require(denied.returncode==64, (denied.stdout,denied.stderr))
    require(output.read_bytes()==original_binary, 'smoke assertion failed: output.read_bytes()==original_binary')
    require(not list(root.glob('.ores-aot.*')), "smoke assertion failed: not list(root.glob('.ores-aot.*'))")
print('source-free driver/Clang/LLD smoke: PASS; NOT Oreslang language parity')
