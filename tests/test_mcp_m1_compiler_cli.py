"""Requires a real built oreslang-llvmc; never count missing compiler as pass."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]

def invocation(binary, fixture, *flags):
    return subprocess.run([str(binary), *flags, str(fixture)],
                          capture_output=True, text=True, timeout=20,
                          env={**os.environ, 'ORES_CORE_LOG_LEVEL':'off'})

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--compiler',required=True,type=Path)
    args=parser.parse_args()
    if not __debug__:
        raise SystemExit('INVALID_TEST_ENVIRONMENT: Python -O disables assertions')
    if not args.compiler.is_file():
        raise SystemExit('NOT_EXECUTED: compiled LLVM driver is missing')
    compiler=args.compiler.resolve()
    runs=[]
    for suffix in ('atto','ores'):
        fixture=ROOT/'tests/fixtures'/f'mcp-descriptor-public.{suffix}'
        result=invocation(compiler,fixture,'--mcp-descriptors')
        if result.returncode != 0:
            raise AssertionError(f'{suffix} descriptor compile failed: {result.stderr[:500]}')
        repeat=invocation(compiler,fixture,'--mcp-descriptors')
        assert repeat.returncode==0 and result.stdout==repeat.stdout
        manifest=json.loads(result.stdout)
        assert manifest['schema']=='atto.mcp.aot-descriptors.m1.v1'
        assert manifest['backend']=='llvm'
        assert manifest['retention']=='opt_in'
        assert manifest['abi']=='atto.mcp.native.descriptor.v1'
        assert manifest['generation_binding']=='host_runtime_required'
        assert re.fullmatch(r'[0-9a-f]{16}',manifest['module_id'])
        assert manifest['executed_guest'] is False
        assert [(x['name'],x['arity']) for x in manifest['descriptors']]== [('add',2),('main',0)]
        assert all(x['public'] is True and x['kind']=='function' and re.fullmatch(r'[0-9a-f]{16}',x['id']) for x in manifest['descriptors'])
        assert len({x['id'] for x in manifest['descriptors']})==len(manifest['descriptors']), 'descriptor ID collision'
        assert 'hidden' not in result.stdout and 'pointer' not in result.stdout
        assert '/tmp/' not in result.stdout and 'password' not in result.stdout
        runs.append(manifest)
    assert runs[0] == runs[1], '.atto and .ores compatibility mismatch'
    fixture=ROOT/'tests/fixtures/mcp-descriptor-public.atto'
    reserved=ROOT/'tests/fixtures/mcp-descriptor-reserved-llvm.atto'
    for flags in (('--mcp-descriptors',), ('--mcp-embed-descriptors',), ()):
        rejected=invocation(compiler,reserved,*flags)
        assert rejected.returncode!=0, f'backend-reserved symbol accepted: {flags}'
        assert not rejected.stdout.strip(), 'invalid guest source leaked descriptor or LLVM IR'
    for flags in (('--mcp-descriptors','--mcp-descriptors'),
                  ('--mcp-descriptors','--constraint-descriptors'),
                  ('--mcp-descriptors','--tree-shake'),
                  ('--mcp-descriptors','--target=wasm32')):
        rejected=invocation(compiler,fixture,*flags)
        assert rejected.returncode!=0, f'unsupported mode accepted: {flags}'
    print('MCP M1 LLVM compiler-backed metadata: .atto+.ores deterministic exports and fail-closed flags PASSED')

if __name__=='__main__':
    main()
