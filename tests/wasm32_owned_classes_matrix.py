#!/usr/bin/env python3
"""Compile actual bounded-class .ores fixtures all the way to wasm32/Node."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--compiler", required=True)
p.add_argument("--clang", required=True)
p.add_argument("--node", required=True)
p.add_argument("--fixtures", required=True)
args = p.parse_args()
root = Path(args.fixtures)

def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, text=True, timeout=35, **kw)

positive = [
    "class-multifield-shared-read.ores",
    "class-multifield-independent-write.ores",
    "class-multifield-mutator.ores",
    "cfg-multifield-guard-true.ores",
]
negative = [
    "class-multifield-overlap-reject.ores",
    "class-mutation-byvalue-reject.ores",
    "class-multifield-shared-write-reject.ores",
]
with tempfile.TemporaryDirectory(prefix="oreslang-wasm32-classes-") as tmp:
    for i, name in enumerate(positive):
        source = root / name
        compiled = run([args.compiler, "--target=wasm32", str(source)])
        assert compiled.returncode == 0, (name, compiled.stderr)
        assert 'target triple = "wasm32-unknown-unknown"' in compiled.stdout
        dest = str(Path(tmp) / f"guest{i}.wasm")
        linked = run([
            args.clang, "--target=wasm32", "-x", "ir", "-Oz", "-nostdlib",
            "-Wl,--no-entry", "-Wl,--export=main",
            "-Wl,--initial-memory=131072", "-Wl,--max-memory=131072",
            "-o", dest, "-"
        ], input=compiled.stdout)
        assert linked.returncode == 0, (name, linked.stderr)
        js = """
import { readFileSync } from 'node:fs';
const bytes=readFileSync(process.argv[1]);
if(!WebAssembly.validate(bytes))process.exit(21);
const module=new WebAssembly.Module(bytes);
if(WebAssembly.Module.imports(module).length)process.exit(22);
const exp=WebAssembly.Module.exports(module);
if(!exp.some(x=>x.kind==='function'&&x.name==='main'))process.exit(23);
const instance=new WebAssembly.Instance(module);
if(instance.exports.main()!==42n)process.exit(24);
"""
        executed = run([args.node, "--input-type=module", "-e", js, dest])
        assert executed.returncode == 0, (name, executed.stderr, executed.returncode)
    for name in negative:
        failed = run([args.compiler, "--target=wasm32", str(root / name)])
        assert failed.returncode != 0 and not failed.stdout, (name, failed.stdout)
    # Deliberately exercise the two unsupported host ABI paths.
    actor = run([args.compiler, "--target=wasm32", str(root / "unsupported-actor.ores")])
    assert actor.returncode != 0 and not actor.stdout
    region = run([args.compiler, "--target=wasm32", "--experimental-owned-int",
                  str(root / "owned-native-region-main.ores")])
    assert region.returncode != 0 and "wasm32 native heap-owned" in region.stderr
    assert not region.stdout
print(json.dumps({"ok": True, "realWasm32Classes": len(positive),
                  "compileTimeRejections": len(negative) + 2}))
