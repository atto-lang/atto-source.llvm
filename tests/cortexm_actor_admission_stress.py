#!/usr/bin/env python3
"""Deterministic model-based Cortex-M actor admission matrix (CI-only harness).

Tests the ORIGINAL pinned LLVM compiler, not a mocked parser. Every invocation
must either produce LLVM IR for a statically safe schedule or fail closed
without producing any IR.  Bounds here follow the pinned Cortex-M contract:
8 actor definitions/spawns, 16 pending messages per actor, 32 sends globally.
"""
import argparse
import pathlib
import random
import subprocess
import tempfile


def actor(name):
    return (f"define actor {name} as {{\n"
            "let total: int = 0;\n"
            "pub fnc on_message(int n): int { total = total + n; return total; }\n"
            "}\n")


def run(compiler, path, source, admitted, label):
    path.write_text(source, encoding="utf-8")
    p = subprocess.run([compiler, "--target=thumbv7m-none-eabi", str(path)],
                       capture_output=True, text=True, timeout=40)
    if admitted:
        if p.returncode != 0 or "oreslang_cortexm_bootstrap" not in p.stdout:
            raise AssertionError(f"{label}: valid schedule rejected: {p.returncode} {p.stderr[:1200]}")
        if "define i32 @main(" in p.stdout:
            raise AssertionError(f"{label}: guest code unexpectedly emitted C main")
    else:
        if p.returncode == 0 or p.stdout:
            raise AssertionError(f"{label}: unsafe schedule admitted; rc={p.returncode}, IR={p.stdout[:500]}")
    return 1


def program(actions, names=("Producer", "Consumer")):
    declarations = "".join(actor(n) for n in names)
    spawns = " ".join(f"const {chr(ord('a')+i)} = spawn {n}();" for i, n in enumerate(names))
    return (declarations + "pub fnc main(): int { " + spawns + " " +
            " ".join(actions) + " return 43; }\n")


def source_for_counts(a, b, pattern):
    if pattern == "grouped":
        actions = ["a.send(1);"] * a + ["b.send(2);"] * b
    else:
        actions = []
        for k in range(max(a, b)):
            if k < a:
                actions.append("a.send(1);")
            if k < b:
                actions.append("b.send(2);")
    actions += ["stop a;", "stop b;"]
    return program(actions)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    args = ap.parse_args()
    total = 0
    with tempfile.TemporaryDirectory(prefix="ores-cortexm-model-") as tmp:
        path = pathlib.Path(tmp) / "matrix.ores"
        # Exhaust all pairs around the exact boundary. An overflowing actor
        # must never be legalized by another actor's unused queue capacity.
        for a in range(18):
            for b in range(18):
                for pattern in ("grouped", "interleaved"):
                    admitted = a <= 16 and b <= 16 and a + b <= 32
                    total += run(args.compiler, path, source_for_counts(a, b, pattern),
                                 admitted, f"queues-{a}-{b}-{pattern}")
        # Mixed stop orders and interleaved enqueues must obey the same
        # per-actor admission constraint independently of ordering.
        rng = random.Random(0xC0F7E)
        for k in range(150):
            a, b = rng.randrange(17), rng.randrange(17)
            actions = ["a.send(1);"] * a + ["b.send(2);"] * b
            rng.shuffle(actions)
            actions.extend(("stop a;", "stop b;") if k % 2 else ("stop b;", "stop a;"))
            total += run(args.compiler, path, program(actions), True, f"shuffle-{k}")
        for n in (0, 1, 15, 16):
            for name, actions in (
                ("double-stop", ["a.send(1);"] * n + ["stop a;", "stop a;", "stop b;"]),
                ("use-after-stop", ["a.send(1);"] * n + ["stop a;", "a.send(2);", "stop b;"]),
                ("missing-stop", ["a.send(1);"] * n + ["stop b;"])):
                total += run(args.compiler, path, program(actions), False, f"{name}-{n}")
        # Compiler's 32-message global limit remains independent of each
        # individual 16-entry queue.
        for distribution in ((11, 11, 11), (16, 16, 1), (15, 15, 3)):
            actions = []
            for idx, n in enumerate(distribution):
                actions.extend([f"{chr(97+idx)}.send(1);"] * n)
            actions.extend(["stop a;", "stop b;", "stop c;"])
            total += run(args.compiler, path,
                         program(actions, ("Producer", "Consumer", "Worker")),
                         False, f"global-33-{distribution}")
    print(f"PASS Cortex-M deterministic actor admission stress: {total}/{total} model cases")


if __name__ == "__main__":
    main()
