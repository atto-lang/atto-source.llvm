"""Fail-closed compiler -> native LLVM guest ELF descriptor retention proof."""
import argparse, hashlib, json, os, re, shutil, subprocess, tempfile
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
    p.add_argument('--report', type=Path, default=None, help='write a success-only exact-source receipt')
    args=p.parse_args()
    # Never let a stale receipt survive a failed or unexecuted run.
    if args.report is not None and args.report.exists():
        args.report.unlink()
    if not __debug__: raise SystemExit('INVALID_TEST_ENVIRONMENT: Python -O disables assertions')
    if not args.compiler.is_file(): raise SystemExit('NOT_EXECUTED: LLVM compiler missing')
    if not shutil.which(args.clang) or not shutil.which(args.objcopy) or not shutil.which('readelf'):
        raise SystemExit('NOT_EXECUTED: clang/objcopy missing')
    compiler=args.compiler.resolve()
    manifests=[]
    executed=[]
    with tempfile.TemporaryDirectory(prefix='atto-m1-aot-') as tmp:
        work=Path(tmp)
        for ext in ('atto','ores'):
            src=ROOT/'tests/fixtures'/f'mcp-descriptor-public.{ext}'
            # Compare bytes in the native executable against an independently
            # invoked compiler metadata mode, not just a hand-written fixture.
            canonical=ok([compiler,'--mcp-descriptors',src]).stdout.encode('utf-8')
            assert canonical and len(canonical)<=8192
            cmd=[compiler,'--mcp-embed-descriptors',src]
            ir=ok(cmd).stdout
            assert ir==ok(cmd).stdout, 'non-deterministic LLVM IR'
            assert '@__atto_mcp_descriptor_v1' in ir and '@llvm.used' in ir
            assert '.atto.mcp.v1' in ir
            source_ir=work/f'{ext}.ll'
            source_ir.write_text(ir)
            for optimization in ('-O0','-O2'):
                binary=work/f'{ext}-{optimization[1:]}-guest'
                ok([args.clang,'-x','ir',optimization,
                    '-ffunction-sections','-fdata-sections',
                    source_ir,'-Wl,--gc-sections','-o',binary])
                result=run([binary])
                assert result.returncode==42, (
                    f'{ext} {optimization} guest returned {result.returncode}: '
                    f'{result.stderr[:300]}')
                read_only_descriptor(binary)
                blob=work/f'{ext}-{optimization[1:]}.bin'
                ok([args.objcopy,'--dump-section',f'.atto.mcp.v1={blob}',binary])
                payload=blob.read_bytes()
                assert payload.endswith(b'\0') and payload.count(b'\0')==1 and len(payload)<=8193
                assert payload[:-1]==canonical, (
                    f'{ext} {optimization}: embedded bytes differ from compiler metadata')
                symbols=ok(['readelf','-Ws',binary]).stdout
                matches=[row for row in symbols.splitlines()
                         if row.split() and row.split()[-1]=='__atto_mcp_descriptor_v1']
                assert any('GLOBAL' in row and 'OBJECT' in row for row in matches), (
                    f'{ext} {optimization}: descriptor symbol not globally visible')
                manifest=json.loads(payload[:-1])
                assert manifest['schema']=='atto.mcp.aot-descriptors.m1.v1'
                assert manifest['abi']=='atto.mcp.native.descriptor.v1'
                assert manifest['generation_binding']=='host_runtime_required'
                assert re.fullmatch(r'[0-9a-f]{16}',manifest['module_id'])
                assert manifest['executed_guest'] is False
                assert [(x['name'],x['arity']) for x in manifest['descriptors']]==[('add',2),('main',0)]
                assert b'hidden' not in payload and b'/tmp/' not in payload
                manifests.append(manifest)
                executed.append({
                    'sourceExtension':ext, 'optimization':optimization,
                    'exitCode':result.returncode,
                    'nativeSha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                    'descriptorSha256':hashlib.sha256(payload).hexdigest(),
                    'compilerMetadataSha256':hashlib.sha256(canonical).hexdigest(),
                    'descriptorBytes':len(payload)
                })
        assert len(manifests)==4 and all(x==manifests[0] for x in manifests), (
            '.atto/.ores or -O0/-O2 descriptor mismatch')
        source=ROOT/'tests/fixtures/mcp-descriptor-public.atto'
        reserved=ROOT/'tests/fixtures/mcp-descriptor-reserved-llvm.atto'
        for flag in ('--mcp-descriptors','--mcp-embed-descriptors'):
            rejected=run([compiler,flag,reserved])
            assert rejected.returncode!=0, f'reserved ABI symbol accepted with {flag}'
            assert not rejected.stdout.strip(), 'rejected source emitted an artifact'
        ordinary=ok([compiler,source]).stdout
        assert '@__atto_mcp_descriptor_v1' not in ordinary and '.atto.mcp.v1' not in ordinary
        ordinary_ir=work/'no-embed.ll'
        ordinary_ir.write_text(ordinary)
        for optimization in ('-O0','-O2'):
            ordinary_binary=work/f'no-embed-{optimization[1:]}-guest'
            ok([args.clang,'-x','ir',optimization,'-fdata-sections',ordinary_ir,
                '-Wl,--gc-sections','-o',ordinary_binary])
            assert run([ordinary_binary]).returncode==42
            assert sections(ordinary_binary)==[], (
                f'default {optimization} compilation leaked metadata')
        for flags in [
          ('--mcp-embed-descriptors','--mcp-embed-descriptors'),
          ('--mcp-embed-descriptors','--mcp-descriptors'),
          ('--mcp-embed-descriptors','--constraint-descriptors'),
          ('--mcp-embed-descriptors','--tree-shake'),
          ('--mcp-embed-descriptors','--target=wasm32'),
          ('--mcp-embed-descriptors','--profile=native-hard-rt')]:
            assert run([compiler,*flags,source]).returncode!=0, f'unsupported mode accepted: {flags}'
    assert len(executed)==4 and len({(x['sourceExtension'],x['optimization']) for x in executed})==4
    assert len({x['descriptorSha256'] for x in executed})==1
    if args.report is not None:
        def git_id(revision):
            p=subprocess.run(['git','-C',str(ROOT),'rev-parse',revision],
                             capture_output=True,text=True,timeout=10)
            if p.returncode: raise AssertionError('no exact Git source identity for receipt')
            return p.stdout.strip()
        head=git_id('HEAD')
        tree=git_id('HEAD^{tree}')
        expected=os.environ.get('EXPECTED_HEAD','')
        if expected and head != expected:
            raise AssertionError('source head differs from trusted workflow head')
        receipt={
            'schema':'atto.mcp.llvm-aot-guest-evidence.v1',
            'evidenceClassification':'COMPILER_BACKED_NATIVE_GUEST_EXECUTED',
            'headSha':head,'treeSha':tree,
            'compilerSha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),
            'cases':executed,
            'defaultRetention':'ABSENT_AT_O0_AND_O2',
            'negativeAdmissions':'PASS',
            'scope':'M1_ELF_GUEST_DESCRIPTOR_ONLY_NOT_MCP_SERVER'
        }
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(receipt,indent=2,sort_keys=True)+'\n')
    print('LLVM compiler -> .atto/.ores O0/O2 native guest + GC-preserved descriptor PASS')
if __name__=='__main__': main()
