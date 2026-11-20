#!/usr/bin/env python3
"""Source-compiled nominal Result local and exhaustive do match gate.

Producer and consumer use internal linkage. The handwritten IR test runner is
appended to the SAME LLVM module; there is no claim about a public C ABI.
"""
import argparse
import re
import signal
from pathlib import Path
import subprocess
import sys
import tempfile

# Runtime fault injection deliberately triggers llvm.trap. Prevent core
# dumps from filling developer/CI disks while exercising negative paths.
try:
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
except (ImportError, OSError, ValueError):
    pass

SOURCE = """
fnc success(): Result<void, ArrayError> { return Ok(); }
fnc limit(): Result<void, ArrayError> { return Err(ArrayError.Limit); }
fnc memory(): Result<void, ArrayError> { return Err(ArrayError.NoMemory); }

fnc from_success(): int {
    const Result<void, ArrayError> result = success();
    do match result over
        on Ok() -> { return 11; }
        on Err(error) -> { return 22; }
    end
}

fnc from_limit(): int {
    const Result<void, ArrayError> response = limit();
    do match response over
        on Err(error) -> { return 33; }
        on Ok() -> { return 44; }
    end
}

fnc from_memory(): int {
    const Result<void, ArrayError> pending = memory();
    do match pending over
        on Ok() -> { return 55; }
        on Err(reason) -> { return 66; }
    end
}

fnc forward_consumer(): int {
    const Result<void, ArrayError> deferred = later_producer();
    do match deferred over
        on Err(error) -> { return 88; }
        on Ok() -> { return 77; }
    end
}
fnc later_producer(): Result<void, ArrayError> { return Ok(); }
"""

CALLER = """
define i32 @main() {
entry:
  %a = call i64 @from_success()
  %b = call i64 @from_limit()
  %c = call i64 @from_memory()
  %d = call i64 @forward_consumer()
  %a_ok = icmp eq i64 %a, 11
  %b_ok = icmp eq i64 %b, 33
  %c_ok = icmp eq i64 %c, 66
  %d_ok = icmp eq i64 %d, 77
  %ab = and i1 %a_ok, %b_ok
  %cd = and i1 %c_ok, %d_ok
  %all = and i1 %ab, %cd
  %code = select i1 %all, i32 0, i32 3
  ret i32 %code
}
"""

PRODUCER = "fnc produce(): Result<void, ArrayError> { return Ok(); }\n"
ARMS = """on Ok() -> { return 1; }
on Err(problem) -> { return 2; }"""
CONSUMER = """fnc use(): int {
    const Result<void, ArrayError> pending = produce();
    do match pending over
        %s
    end
}
"""

NEGATIVE = {
    "missing-err": (
        PRODUCER + CONSUMER % "on Ok() -> { return 1; }",
        "non-exhaustive native Result match"),
    "reserved-result-producer": (
        "fnc Result(): Result<void, ArrayError> { return Ok(); }",
        "native tagged Result name collides"),
    "reserved-variant-producer": (
        "fnc Ok(): Result<void, ArrayError> { return Ok(); }",
        "native tagged Result name collides"),
    "reserved-variant-consumer": (
        PRODUCER + (CONSUMER % ARMS).replace("fnc use():", "fnc Err():"),
        "native Result match name collides"),
    "missing-ok": (
        PRODUCER + CONSUMER % "on Err(problem) -> { return 2; }",
        "non-exhaustive native Result match"),
    "err-shadows-owner": (
        PRODUCER + CONSUMER %
        "on Ok() -> { return 1; } on Err(pending) -> { return 2; }",
        "Err binding cannot shadow the Result owner"),
    "err-shadows-producer": (
        PRODUCER + CONSUMER %
        "on Ok() -> { return 1; } on Err(produce) -> { return 2; }",
        "Err binding cannot shadow the Result owner"),
    "err-shadows-nominal": (
        PRODUCER + CONSUMER %
        "on Ok() -> { return 1; } on Err(ArrayError) -> { return 2; }",
        "Err binding cannot shadow the Result owner"),
    "duplicate-ok": (
        PRODUCER + CONSUMER %
        "on Ok() -> { return 1; } on Ok() -> { return 2; }",
        "duplicate nominal Result match arm"),
    "duplicate-err": (
        PRODUCER + CONSUMER %
        "on Err(a) -> { return 1; } on Err(b) -> { return 2; }",
        "duplicate nominal Result match arm"),
    "wildcard-instead-of-err": (
        PRODUCER + CONSUMER %
        "on Ok() -> { return 1; } on _ -> { return 2; }",
        "requires nominal Ok()/Err(error) arms"),
    "unknown-producer": (
        CONSUMER.replace("produce()", "nonexistent()") % ARMS,
        "requires a known closed Result<void,ArrayError> producer"),
    "producer-is-scalar": (
        "fnc produce(): int { return 10; }\n" + CONSUMER % ARMS,
        "requires a known closed Result<void,ArrayError> producer"),
    "match-wrong-local": (
        PRODUCER + (CONSUMER % ARMS).replace(
            "do match pending over", "do match other over"),
        "must consume its exact named local"),
    "drop-without-match": (
        PRODUCER + """fnc use(): int {
        const Result<void, ArrayError> pending = produce();
        return 0;
    }""", "expected 'do'"),
    "nonterminal-return": (
        PRODUCER + (CONSUMER % ARMS).replace(
            "end\n}", "end\nreturn 0;\n}"),
        "expected '}'"),
    "unverified-public-consumer": (
        PRODUCER + ("pub " + CONSUMER % ARMS),
        "requires private, unqualified zero-argument int function"),
    "unverified-effectful-consumer": (
        PRODUCER + (CONSUMER % ARMS).replace(
            "fnc use(): int", "fnc use(): int using alloc"),
        "requires private, unqualified zero-argument int function"),
    "consumer-name-collision": (
        PRODUCER + CONSUMER % ARMS + """fnc use(): int { return 0; }""",
        "native Result match name collides"),
    "consumer-producer-collision": (
        PRODUCER + (CONSUMER % ARMS).replace("fnc use():", "fnc produce():"),
        "native Result match name collides"),
    "result-move-no-match": (
        PRODUCER + (CONSUMER % ARMS).replace(
            "do match pending over", "const int another = 42; do match pending over"),
        "expected 'do'"),
}

def run(args, timeout=30):
    return subprocess.run(args, capture_output=True, check=False, timeout=timeout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-result-match-") as folder:
        root = Path(folder)
        source = root / "positive.ores"
        source.write_text(SOURCE, encoding="utf-8")
        compiled = run([str(args.compiler), str(source)], timeout=45)
        if compiled.returncode != 0 or not compiled.stdout.startswith(b"; ModuleID"):
            raise AssertionError(
                f"source compiler failed: {compiled.returncode} {compiled.stderr!r}")
        ir = compiled.stdout.decode("utf-8")
        for name in ("from_success", "from_limit", "from_memory", "forward_consumer"):
            if ("define internal i64 @" + name + "()") not in ir:
                raise AssertionError("missing private typed Result consumer: " + name)
        for fragment in (
                "%ores.result.void.array_error = type { i8, i8 }",
                "result.typed.slot", "result.variant", "result.error",
                "result.invalid", "llvm.trap", "result.ok", "result.err"):
            if fragment not in ir:
                raise AssertionError("missing checked typed local or match IR: " + fragment)
        ll = root / "guest.ll"
        ll.write_bytes(compiled.stdout + b"\n" + CALLER.encode("utf-8"))
        for optimization in ("-O0", "-O2"):
            executable = root / ("native-" + optimization[1:])
            built = run([str(args.clang), "-Werror", "-Wno-override-module",
                         optimization, "-x", "ir", str(ll), "-o", str(executable)],
                        timeout=45)
            if built.returncode:
                raise AssertionError(f"clang failed: {built.stderr!r}")
            ran = run([str(executable)])
            if ran.returncode:
                raise AssertionError(
                    f"Result match branches incorrect {optimization}: "
                    f"status={ran.returncode} stderr={ran.stderr!r}")
        # Exercise the runtime invalid-tag trap, not merely the presence of
        # its LLVM intrinsic in a text search. Corrupt the closed producer
        # inside the generated LLVM module, preserving every other instruction.
        signature = "define internal %ores.result.void.array_error @limit()"
        start = ir.find(signature)
        if start < 0:
            raise AssertionError("missing nominal producer for trap injection")
        end = ir.find("\n}", start)
        if end < 0:
            raise AssertionError("unterminated producer LLVM definition")
        prefix, body, suffix = ir[:start], ir[start:end], ir[end:]
        for fault, payload in (
                ("invalid-variant", "{ i8 7, i8 1 }"),
                ("unknown-error", "{ i8 1, i8 7 }")):
            mutated, count = re.subn(
                r"\{\s*i8\s+1\s*,\s*i8\s+1\s*\}",
                payload, body, count=1)
            if count != 1:
                raise AssertionError(
                    "LLVM closed producer IR did not contain exactly one "
                    "canonical recoverable-error return")
            corrupt = root / (fault + ".ll")
            corrupt.write_text(prefix + mutated + suffix + "\n" + CALLER,
                               encoding="utf-8")
            for optimization in ("-O0", "-O2"):
                bad_exe = root / (fault + "-" + optimization[1:])
                built = run([str(args.clang), "-Werror",
                             "-Wno-override-module", optimization, "-x",
                             "ir", str(corrupt), "-o", str(bad_exe)], timeout=45)
                if built.returncode:
                    raise AssertionError(
                        f"{fault} malformed injected LLVM IR: {built.stderr!r}")
                crashed = run([str(bad_exe)])
                # A nonzero result is NOT evidence of a trap: the wrong
                # branch could simply return an ordinary nonzero status.
                # On POSIX the LLVM trap intrinsic must terminate via a
                # trap/illegal-instruction signal, not by normal return.
                if crashed.returncode not in (-signal.SIGILL, -signal.SIGTRAP):
                    raise AssertionError(
                        f"{fault}: expected actual hardware trap at {optimization}, "
                        f"got ordinary exit/signal {crashed.returncode}; "
                        f"stderr={crashed.stderr!r}")

        for label, (code, diagnostic) in NEGATIVE.items():
            path = root / (label + ".ores")
            path.write_text(code, encoding="utf-8")
            checked = run([str(args.compiler), str(path)])
            if checked.returncode != 1 or checked.stdout or (
                    diagnostic.encode("utf8") not in checked.stderr):
                raise AssertionError(
                    f"{label}: expected precise fail-closed diagnostic "
                    f"{diagnostic!r}; rc={checked.returncode}, "
                    f"out={checked.stdout!r}, err={checked.stderr!r}")
    print("PASS: source typed Result local, exhaustive match and no-IR negatives")


if __name__ == "__main__":
    try:
        main()
    except (OSError, AssertionError, subprocess.TimeoutExpired) as exc:
        print("native closed Result match FAILED:", exc, file=sys.stderr)
        sys.exit(1)
