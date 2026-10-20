"""Compile the preserved scalar-field proposal with the current actor ABI."""
import pathlib, subprocess, sys, tempfile
compiler, clang, root = sys.argv[1:]
root = pathlib.Path(root)
with tempfile.TemporaryDirectory() as tmp:
    ir = subprocess.run([compiler, str(root / 'tests/fixtures/actor-owned-fields.ores')], capture_output=True, text=True, timeout=15)
    assert ir.returncode == 0, ir.stderr
    ll = pathlib.Path(tmp) / 'actor.ll'; ll.write_text(ir.stdout)
    exe = pathlib.Path(tmp) / 'actor'
    build = subprocess.run([clang, str(ll), str(root / 'tests/native_actor_source_harness.c'), str(root / 'src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c'), '-I'+str(root / 'src/main/cpp'), '-o', str(exe)], capture_output=True, text=True, timeout=45)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
    assert run.returncode == 0, (run.returncode, run.stderr)
