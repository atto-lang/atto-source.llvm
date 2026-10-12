"""Execute a qualified module entry without inventing an unchecked process ABI."""
import pathlib, subprocess, sys, tempfile
compiler, clang, root = sys.argv[1:]
root = pathlib.Path(root)
with tempfile.TemporaryDirectory() as tmp:
    ir = subprocess.run([compiler, str(root / 'tests/fixtures/native-contract-i64-valid.ores')], capture_output=True, text=True, timeout=15)
    assert ir.returncode == 0, ir.stderr
    ll = pathlib.Path(tmp) / 'module.ll'; ll.write_text(ir.stdout)
    shim = pathlib.Path(tmp) / 'probe.c'
    shim.write_text('#include <stdint.h>\n#ifdef __APPLE__\nextern int64_t entry(void) __asm__("_Math.main");\n#else\nextern int64_t entry(void) __asm__("Math.main");\n#endif\nint main(void) { return entry() == 42 ? 0 : 1; }\n')
    exe = pathlib.Path(tmp) / 'probe'
    link = subprocess.run([clang, str(ll), str(shim), '-o', str(exe)], capture_output=True, text=True, timeout=30)
    assert link.returncode == 0, link.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
    assert run.returncode == 0, (run.returncode, run.stderr)
