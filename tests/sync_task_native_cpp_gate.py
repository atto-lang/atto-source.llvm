#!/usr/bin/env python3
"""Native C++ parser must reject unsupported sync task before emitting LLVM IR.

This is intentionally *not* a claim of executable C++ inline-task support.
The executable Rust atto-llvm backend is tested separately.
"""
import pathlib
import subprocess
import sys
import tempfile

def test(compiler: str, name: str, source: str, *, supported: bool):
    with tempfile.TemporaryDirectory(prefix="atto-cpp-sync-") as directory:
        path = pathlib.Path(directory) / f"{name}.atto"
        path.write_text(source)
        result = subprocess.run(
            [compiler, "--target=host", str(path)],
            capture_output=True, text=True, check=False,
        )
        if supported:
            assert result.returncode == 0, (name, result.stderr)
            assert "define " in result.stdout, (name, result.stdout[:300])
        else:
            assert result.returncode != 0, (name, result.stdout)
            assert not result.stdout, (name, result.stdout[:300])
            assert "E_UNSUPPORTED_SYNC_TASK" in result.stderr, (name, result.stderr)

def main():
    compiler = sys.argv[1]
    test(compiler, "simple", "pub fnc main(): int { val int n = sync task<int> { return 4; }; return n; }", supported=False)
    test(compiler, "empty", "pub fnc main(): int { sync task {}; return 4; }", supported=False)
    test(compiler, "void", "pub fnc main(): int { do sync task {}; return 4; }", supported=False)
    test(compiler, "reserved", "pub fnc main(): int { val int sync = 1; return sync; }", supported=False)
    test(compiler, "comments", "pub fnc main(): int { // sync task {}\n return 7; }", supported=True)
    test(compiler, "prefix", "pub fnc main(): int { val int synchronize = 8; return synchronize; }", supported=True)
    print("ATTO_CPP_SYNC_TASK_LEXICAL_FAIL_CLOSED_PASS=6")

if __name__ == "__main__":
    main()
