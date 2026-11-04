"""Source-free linker-retention canary. NOT compiled AttoLang guest parity."""
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

PAYLOAD=b'{"schema":"atto.mcp.aot-descriptors.m1.v1","abi":"atto.mcp.native.descriptor.v1"}\n\0'

def execute(args):
    p=subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=30)
    if p.returncode: raise AssertionError(f"{args[0]} failed: {p.stderr[:800]}")
    return p

def verify_read_only(binary):
    sections=subprocess.run(['readelf','-W','-S',str(binary)],capture_output=True,text=True,timeout=30)
    if sections.returncode:
        raise AssertionError(f'readelf failed: {sections.stderr[:300]}')
    matching=[]
    for line in sections.stdout.splitlines():
        match=re.match(r'^\s*\[\s*\d+\]\s+\.atto\.mcp\.v1\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+',line)
        if match: matching.append((match.group(1),match.group(6)))
    if len(matching)!=1 or matching[0][0]!='PROGBITS' or 'A' not in matching[0][1] or any(c in matching[0][1] for c in 'WX'):
        raise AssertionError(f'descriptor must be retained once in read-only allocated ELF section: {matching}')

def main():
    if not __debug__:
        raise SystemExit('INVALID_TEST_ENVIRONMENT: Python -O disables assertions')
    if not shutil.which('clang') or not shutil.which('objcopy') or not shutil.which('readelf'):
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
        verify_read_only(exe)
        blob=root/'payload.bin'
        execute(['objcopy','--dump-section',f'.atto.mcp.v1={blob}',exe])
        assert blob.read_bytes()==PAYLOAD
        # Negative mutation: a writable descriptor section must fail admission.
        # This test ensures read-only verification is not merely a no-op.
        writable_ir=root/'writable.ll'
        writable_ir.write_text(ir.replace(' = constant [',' = global ['))
        writable=root/'writable'
        execute(['clang','-x','ir','-O2','-fdata-sections',
                 writable_ir,'-Wl,--gc-sections','-o',writable])
        try:
            verify_read_only(writable)
        except AssertionError:
            pass
        else:
            raise AssertionError('writable descriptor section falsely accepted')
    print('Source-free llvm.used ELF --gc-sections linker-retention canary PASS')

if __name__=='__main__': main()
