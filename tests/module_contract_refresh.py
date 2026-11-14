"""Execute the module through the compiler's checked i32 process wrapper."""
import pathlib, subprocess, sys, tempfile
compiler, clang, root = sys.argv[1:]
root = pathlib.Path(root)
with tempfile.TemporaryDirectory() as tmp:
    ir = subprocess.run([compiler, str(root / 'tests/fixtures/native-contract-i64-valid.ores')], capture_output=True, text=True, timeout=15)
    assert ir.returncode == 0, ir.stderr
    assert 'define i32 @main()' in ir.stdout, ir.stdout
    ll = pathlib.Path(tmp) / 'module.ll'; ll.write_text(ir.stdout)
    exe = pathlib.Path(tmp) / 'probe'
    link = subprocess.run([clang, str(ll), '-o', str(exe)], capture_output=True, text=True, timeout=30)
    assert link.returncode == 0, link.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
    assert run.returncode == 42, (run.returncode, run.stderr)
