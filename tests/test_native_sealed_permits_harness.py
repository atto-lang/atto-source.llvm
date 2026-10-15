#!/usr/bin/env python3
"""Meta-test: reject false-positive sealed-compiler proof from the test harness."""
from pathlib import Path
import subprocess
import sys
import tempfile

HARNESS = Path(__file__).with_name("native_sealed_permits_fail_closed.py")

DIAGNOSTIC = "native sealed/final/permits hierarchy unsupported"

MOCKS = {
    "correct-failure": (f"print({DIAGNOSTIC!r}, file=sys.stderr)\nsys.exit(1)\n", True, ""),
    "leaked-ir-file": (
        f"Path('leak.ll').write_text('define i32 @main() {{ ret i32 0 }}')\n"
        f"print({DIAGNOSTIC!r}, file=sys.stderr)\nsys.exit(1)\n",
        False, "unexpected_files=['leak.ll']"
    ),
    "leaked-stdout-ir": (
        f"print('define i32 @main() {{ ret i32 0 }}')\n"
        f"print({DIAGNOSTIC!r}, file=sys.stderr)\nsys.exit(1)\n",
        False, "stdout="
    ),
    "unrelated-parse-failure": (
        "print('unrelated parser failure', file=sys.stderr)\nsys.exit(1)\n",
        False, "without emitting IR or files"
    ),
    "wrong-exit-code": (
        f"print({DIAGNOSTIC!r}, file=sys.stderr)\nsys.exit(0)\n",
        False, "rc=0"
    ),
}

def main() -> None:
    with tempfile.TemporaryDirectory(prefix="ores-sealed-harness-meta-") as temp:
        for name, (body, should_pass, marker) in MOCKS.items():
            fake_compiler = Path(temp) / (name + ".py")
            fake_compiler.write_text(
                "#!/usr/bin/env python3\nimport sys\nfrom pathlib import Path\n" + body,
                encoding="utf-8"
            )
            fake_compiler.chmod(0o755)
            completed = subprocess.run(
                [sys.executable, str(HARNESS), str(fake_compiler)],
                capture_output=True, text=True, timeout=25, check=False,
            )
            actual_success = completed.returncode == 0
            if actual_success != should_pass or (marker and marker not in completed.stderr):
                raise AssertionError(
                    f"{name}: expected success={should_pass}, got {completed.returncode}; "
                    f"stdout={completed.stdout!r}, stderr={completed.stderr!r}"
                )
    print(f"PASS: {len(MOCKS)} sealed fail-closed harness meta-tests")

if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"sealed harness self-test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
