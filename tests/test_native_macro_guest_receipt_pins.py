#!/usr/bin/env python3
"""Negative-only CMake receipt mutation tests. NOT real Atto compiler proof."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

IR = "define i64 @main() {\nentry:\n  ret i64 42\n}\n"
EXPECTED = {
    "consumer": "macro consumer source changed",
    "compiler": "macro compiler binary changed",
    "producer": "macro producer changed",
}


def check_mutation(case: str, cmake: str, clang: str, gate: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="atto-macro-receipt-negative-") as tmp:
        root = Path(tmp)
        consumer = root / "guest.atto"
        producer = root / "producer.atto"
        compiler = root / "fake-compiler.py"
        consumer.write_text("pub fnc main() -> int {return 42;}\n", encoding="utf-8")
        producer.write_text("define pub macro literal() {}\n", encoding="utf-8")
        target = {"consumer": consumer, "compiler": compiler, "producer": producer}[case]
        # The controlled stub exists ONLY to force an input mutation while the
        # CMake receipt verifier runs; it is not an Atto compiler or guest.
        program = (
            "#!" + sys.executable + "\n"
            "from pathlib import Path\n"
            "import sys\n"
            + "path = Path(" + repr(str(target)) + ")\n"
            + "path.write_bytes(path.read_bytes() + b'\\n# tampered during invocation\\n')\n"
            + "sys.stdout.write(" + repr(IR) + ")\n"
        )
        compiler.write_text(program, encoding="utf-8")
        compiler.chmod(0o700)
        command = [
            cmake, "-DCOMPILER=" + str(compiler),
            "-DFIXTURE=" + str(consumer),
            "-DPRODUCER=" + str(producer),
            "-DCLANG=" + clang,
            "-DSTAGING=" + str(root / "stage"),
            "-P", str(gate),
        ]
        run = subprocess.run(command, text=True, capture_output=True,
                             check=False, timeout=65)
        output = run.stdout + run.stderr
        if run.returncode == 0 or EXPECTED[case] not in output:
            raise AssertionError(
                f"{case}: mutation receipt was not rejected: "
                f"return={run.returncode} output={output[-1200:]!r}"
            )
        if "MACRO_LLVM_LINKED_GUEST_AOT_PASS" in output:
            raise AssertionError(f"{case}: forged positive receipt marker")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--receipt-script", type=Path, required=True)
    args = parser.parse_args()
    if not args.receipt_script.is_file():
        raise AssertionError("the production CMake receipt script is required")
    if not shutil.which(args.cmake) or not shutil.which(args.clang):
        raise AssertionError("real CMake and Clang are required for the receipt negative test")
    for case in EXPECTED:
        check_mutation(case, args.cmake, args.clang, args.receipt_script)
    print("MACRO_RECEIPT_MUTATION_DENIAL_PASS cases=3 (synthetic compiler, no native Atto proof)")


if __name__ == "__main__":
    main()
