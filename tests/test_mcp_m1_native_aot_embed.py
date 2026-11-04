"""Fail-closed compiler -> native LLVM guest ELF descriptor retention proof."""
import argparse, json, re, shutil, subprocess, tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
def run(cmd):
    return subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=45)
def ok(cmd):
    p=run(cmd)
    if p.returncode: raise AssertionError(f"{cmd[0]} failed {p.returncode}: {p.stderr[:500]}")
    return p
def sections(binary):
    out=ok(['readelf','-W','-S',binary]).stdout
    rows=[]
    for line in out.splitlines():
        match=re.match(
            r'^\s*\[\s*\d+\]\s+\.atto\.mcp\.v1\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+',
            line)
        if match:
            rows.append((match.group(1),match.group(6)))
    return rows

def read_only_descriptor(binary):
    found=sections(binary)
    assert len(found)==1, f'expected one ELF descriptor section: {found}'
    kind, flags=found[0]
    assert kind=='PROGBITS' and 'A' in flags and 'W' not in flags and 'X' not in flags, \
        f'unsafe ELF MCP descriptor flags: {found}'

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--compiler',type=Path,required=True)
    p.add_argument('--clang',default='clang')
    p.add_argument('--objcopy',default='objcopy')
    args=p.parse_args()
    if not __debug__: raise SystemExit('INVALID_TEST_ENVIRONMENT: Python -O disables assertions')
    if not args.compiler.is_file(): raise SystemExit('NOT_EXECUTED: LLVM compiler missing')
    if not shutil.which(args.clang) or not shutil.which(args.objcopy) or not shutil.which('readelf'):
        raise SystemExit('NOT_EXECUTED: clang/objcopy missing')
    compiler=args.compiler.resolve()
    manifests=[]
    with tempfile.TemporaryDirectory(prefix='atto-m1-aot-') as tmp:
        work=Path(tmp)
        for ext in ('atto','ores'):
            src=ROOT/'tests/fixtures'/f'mcp-descriptor-public.{ext}'
            cmd=[compiler,'--mcp-embed-descriptors',src]
            ir=ok(cmd).stdout
            assert ir==ok(cmd).stdout, 'non-deterministic LLVM IR'
            assert '@__atto_mcp_descriptor_v1' in ir and '@llvm.used' in ir
            assert '.atto.mcp.v1' in ir
            source_ir=work/f'{ext}.ll'
            source_ir.write_text(ir)
            binary=work/f'{ext}-guest'
            ok([args.clang,'-x','ir','-O2','-ffunction-sections','-fdata-sections',
                source_ir,'-Wl,--gc-sections','-o',binary])
            result=run([binary])
            assert result.returncode==42, f'guest returned {result.returncode}: {result.stderr[:300]}'
            read_only_descriptor(binary)
            blob=work/f'{ext}.bin'
            ok([args.objcopy,'--dump-section',f'.atto.mcp.v1={blob}',binary])
            payload=blob.read_bytes()
            assert payload.endswith(b'\0') and payload.count(b'\0')==1 and len(payload)<=8193
            manifest=json.loads(payload[:-1])
            assert manifest['schema']=='atto.mcp.aot-descriptors.m1.v1'
            assert manifest['abi']=='atto.mcp.native.descriptor.v1'
            assert manifest['generation_binding']=='host_runtime_required'
            assert re.fullmatch(r'[0-9a-f]{16}',manifest['module_id'])
            assert manifest['executed_guest'] is False
            assert [(x['name'],x['arity']) for x in manifest['descriptors']]==[('add',2),('main',0)]
            assert b'hidden' not in payload and b'/tmp/' not in payload
            manifests.append(manifest)
        assert manifests[0]==manifests[1], '.atto vs .ores metadata mismatch'
        source=ROOT/'tests/fixtures/mcp-descriptor-public.atto'
        ordinary=ok([compiler,source]).stdout
        assert '@__atto_mcp_descriptor_v1' not in ordinary and '.atto.mcp.v1' not in ordinary
        ordinary_ir=work/'no-embed.ll'
        ordinary_ir.write_text(ordinary)
        ordinary_binary=work/'no-embed-guest'
        ok([args.clang,'-x','ir','-O2','-fdata-sections',ordinary_ir,
            '-Wl,--gc-sections','-o',ordinary_binary])
        assert run([ordinary_binary]).returncode==42
        assert sections(ordinary_binary)==[], 'default compilation leaked metadata'
        for flags in [
          ('--mcp-embed-descriptors','--mcp-embed-descriptors'),
          ('--mcp-embed-descriptors','--mcp-descriptors'),
          ('--mcp-embed-descriptors','--constraint-descriptors'),
          ('--mcp-embed-descriptors','--tree-shake'),
          ('--mcp-embed-descriptors','--target=wasm32'),
          ('--mcp-embed-descriptors','--profile=native-hard-rt')]:
            assert run([compiler,*flags,source]).returncode!=0, f'unsupported mode accepted: {flags}'
    print('LLVM compiler -> .atto/.ores native guest + GC-preserved descriptor PASS')
if __name__=='__main__': main()
