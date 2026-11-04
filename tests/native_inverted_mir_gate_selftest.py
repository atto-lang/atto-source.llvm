#!/usr/bin/env python3
"""Adversarial self-test of the LLVM IR guest shell gate, NOT guest compiler proof.

The emitter below is deliberately synthetic; it exercises both O0/O2 native
execution and checks that a non-signal exit or unrelated diagnostic cannot be
mistaken for passing the actual MIR guest verification gate.
"""
import argparse
import os
from pathlib import Path
import shutil
import tempfile
import subprocess
import sys

ERRORS = {
    "immutable": "immutable inverted field cannot be written after initialization",
    "missing-operand": "missing MIR LLVM push operands",
    "unclosed": "MIR LLVM requires terminal owner close",
    "snapshot": "MIR LLVM snapshot value lowering not implemented",
    "no-read": "MIR LLVM requires terminal field read before close",
    "stale-read": "MIR LLVM requires terminal field read before close",
}


def emit_mock(mode: str, dest: str) -> int:
    if mode in ERRORS:
        if os.getenv("MIR_GATE_SIMULATION") == "bad-diagnostic" and mode == "stale-read":
            print("unrelated compiler failure", file=sys.stderr)
        else:
            print(ERRORS[mode], file=sys.stderr)
        return 3
    if mode not in {"positive", "closed", "bounds"}:
        print(f"unsupported synthetic fixture: {mode}", file=sys.stderr)
        return 3
    fake_trap = os.getenv("MIR_GATE_SIMULATION") == "false-trap" and mode == "bounds"
    # A false-trap candidate keeps llvm.trap in the IR, but it is on a dead
    # branch; the process exits normally with 132. Only waitpid signal status
    # can distinguish this from an actual SIGILL/SIGTRAP delivery.
    bound = "0, 1" if fake_trap or mode != "bounds" else "1, 1"
    code = """; SYNTHETIC IR: test gate behavior only, not AttoLang guest lowering
declare void @llvm.trap()
define i32 @main() {
entry:
  %alive = alloca i1
  store i1 true, ptr %alive
  %length = alloca i64
  store i64 1, ptr %length
  %col0 = alloca [2 x i64]
  %col1 = alloca [2 x i64]
  %generation = alloca [2 x i64]
  %owner_ok = load i1, ptr %alive
  br i1 %owner_ok, label %a, label %trap
trap:
  call void @llvm.trap()
  unreachable
a:
  %in_bounds = icmp ult i64 """ + bound + """
  br i1 %in_bounds, label %b, label %trap
b:
  %generation_ok = icmp ne i64 1, 0
  br i1 %generation_ok, label %valid, label %trap
valid:
  store volatile i1 false, ptr %alive
"""
    for lane in ("col0", "col1", "generation"):
        for index in range(2):
            code += (f"  %p_{lane}_{index} = getelementptr inbounds [2 x i64], "
                     f"ptr %{lane}, i64 0, i64 {index}\n")
            code += f"  store volatile i64 0, ptr %p_{lane}_{index}\n"
    code += "  store volatile i64 0, ptr %length\n"
    code += f"  ret i32 {132 if fake_trap else 42}\n}}\n"
    Path(dest).write_text(code, encoding="utf-8")
    return 0


def test_gate(shell: str, clang: str) -> int:
    cases = (
        ("", True, "O0/O2 exit 42"),
        ("bad-diagnostic", False, "wrong compiler diagnostic for stale-read"),
        ("false-trap", False, "expected a real SIGILL/SIGTRAP, got 132"),
    )
    # GitHub contents API creates non-executable source files by default.
    # Run an executable *temporary copy* of this synthetic emitter instead of
    # changing permissions or bytes of the checked-out repository.
    with tempfile.TemporaryDirectory(prefix="mir-gate-synthetic-") as work:
        emitter = Path(work) / "mock_emitter.py"
        shutil.copyfile(Path(__file__).resolve(), emitter)
        emitter.chmod(0o700)
        for simulation, should_pass, expected in cases:
            env = dict(os.environ)
            env["MIR_GATE_SIMULATION"] = simulation
            try:
                result = subprocess.run(
                    ["bash", shell, str(emitter), clang],
                    env=env, text=True, capture_output=True, check=False, timeout=120)
            except (OSError, subprocess.TimeoutExpired) as exc:
                raise RuntimeError(f"synthetic gate {simulation or 'valid'} did not execute: {exc}") from exc
            success = result.returncode == 0
            output = result.stdout + "\n" + result.stderr
            if success != should_pass or expected not in output:
                raise RuntimeError(f"gate {simulation or 'valid'}: rc={result.returncode}, "
                                   f"expected success={should_pass}, marker={expected!r}\n{output[-1800:]}")
    print("PASS: source-free MIR gate self-test (O0/O2, exact diagnostic, false exit-132)")
    return 0


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] in ERRORS.keys() | {"positive", "closed", "bounds"}:
        return emit_mock(sys.argv[1], sys.argv[2])
    parser = argparse.ArgumentParser()
    parser.add_argument("--shell", required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    return test_gate(args.shell, args.clang)


if __name__ == "__main__":
    raise SystemExit(main())
