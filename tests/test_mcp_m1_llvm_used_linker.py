"""Source-free linker-retention canary. NOT compiled AttoLang guest parity."""
import shutil
import subprocess
import tempfile
from pathlib import Path

PAYLOAD=b'{"schema":"atto.mcp.aot-descriptors.m1.v1","abi":"atto.mcp.native.descriptor.v1"}\n\0'

def execute(args):
    p=subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=30)
    if p.returncode: raise AssertionError(f"{args[0]} failed: {p.stderr[:800]}")
    return p

def main():
    if not shutil.which('clang') or not shutil.which('objcopy'):
        raise SystemExit("NOT_EXECUTED: clang or objcopy missing")
    payload=''.join(f'\\{b:02X}' for b in PAYLOAD)
    ir=f'''@__atto_mcp_descriptor_v1 = constant [{len(PAYLOAD)} x i8] c"{payload}", section ".atto.mcp.v1", align 1
@llvm.used = appending global [1 x ptr] [ptr @__atto_mcp_descriptor_v1], section "llvm.metadata"
define i32 @main() {{ ret i32 0 }}
'''
    with tempfile.TemporaryDirectory(prefix='atto-mcp-llvm-used-') as d:
        root=Path(d)
        ir_path=root/'probe.ll'
        ir_path.write_text(ir)
        exe=root/'probe'
        execute(['clang','-x','ir','-O2','-fdata-sections',ir_path,'-Wl,--gc-sections','-o',exe])
        assert subprocess.run([str(exe)],capture_output=True).returncode==0
        blob=root/'payload.bin'
        execute(['objcopy','--dump-section',f'.atto.mcp.v1={blob}',exe])
        assert blob.read_bytes()==PAYLOAD
    print('Source-free llvm.used ELF --gc-sections linker-retention canary PASS')

if __name__=='__main__': main()
