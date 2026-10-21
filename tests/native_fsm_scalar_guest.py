#!/usr/bin/env python3
"""Compile and execute real scalar Oreslang guest FSM fixture with LLVM.

No 'skips': absent compiler, compilation, link, execution or admission evidence
fails. Only state/guard/rejection semantics are checked here, not effects,
versions, ECS or region capabilities. OS exit status encodes result + 1.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def require(condition, message):
    # Unlike assert, this evidence gate cannot disappear under python -O.
    if not condition:
        raise AssertionError(message)


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(argv, timeout=30):
    return subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, timeout=timeout, check=False)


def expected(state, event, ticks):
    if ticks not in (0, 1, 2, 3):
        return -1
    if state == 0 and event == 0:
        return 1
    if state == 1 and event == 1 and ticks < 3:
        return 1
    if state == 1 and event == 2 and ticks >= 2:
        return 2
    if state == 2 and event == 3:
        return 0
    return -1


def load_trace_oracle(path):
    require(path is not None and path.is_file(), "missing shared FSM trace oracle")
    spec = importlib.util.spec_from_file_location("ore_fsm_trace_contract", path)
    require(spec is not None and spec.loader is not None,
            "unloadable shared FSM trace oracle")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    require(module.CONTRACT == "ores-fsm-trace-v0",
            "unsupported shared FSM trace contract")
    return module


def oracle_expected(oracle, state, event, ticks):
    # Shared contract models *unsigned* snapshots and events.
    # Negative out-of-domain guest probes are checked by expected() only.
    require(all(type(v) is int for v in (state, event, ticks)),
            "guest FSM oracle inputs must be integers, not bool/coerced values")
    if any(v < 0 or v > oracle.LIMIT for v in (state, event, ticks)):
        return None
    before = oracle.snap(state, 0, ticks)
    outcome = oracle.step(before, event)
    require(type(outcome) is tuple and len(outcome) == 4,
            "malformed shared trace oracle tuple")
    accepted, reason, after, effects = outcome
    require(type(accepted) is bool and type(reason) is str
            and type(after) is dict and type(effects) is list,
            "malformed shared trace oracle outcome")
    require(set(after) == {"state", "version", "ticks"}
            and all(type(v) is int and 0 <= v <= oracle.LIMIT for v in after.values()),
            "malformed shared trace oracle snapshot")
    require(reason in ("none", "invalid-state", "invalid-ticks",
                       "version-exhausted", "guard", "unhandled"),
            "unknown shared trace oracle rejection reason")
    if accepted:
        require(reason == "none" and after["version"] == 1
                and len(effects) == 1
                and type(effects[0]) is int and 1 <= effects[0] <= 4,
                "malformed accepted shared trace oracle effect/version")
    else:
        require(reason != "none" and after == before and effects == [],
                "rejected shared trace oracle transition mutated state or emitted effects")
    return after["state"] if accepted else -1


def verify(root, compiler, clang, trace_oracle=None):
    fixture = root / "tests/fixtures/fsm-scalar-reducer.ores"
    negative = root / "tests/fixtures/fsm-duplicate-function-reject.ores"
    require(compiler.is_file() and clang.is_file(), "LLVM or Clang compiler unavailable")
    require(os.access(compiler, os.X_OK) and os.access(clang, os.X_OK),
            "LLVM or Clang compiler not executable")
    require(fixture.is_file() and negative.is_file(), "missing FSM guest fixtures")
    require(fixture.resolve().is_relative_to(root) and negative.resolve().is_relative_to(root),
            "FSM guest fixture escapes source root")
    prelude = fixture.read_text(encoding="utf-8")
    require("pub fnc main(" not in prelude and len(prelude) < 4096,
            "fixture exceeds bound or already defines main")
    oracle = load_trace_oracle(trace_oracle) if trace_oracle else None
    oracle_cases = 0
    observed = []
    # Original 100 canonical cases plus adversarial packed-coordinate
    # aliases: state*10+event must never stand in for a pair of identities.
    cases = [(state, event, ticks)
             for state in (0, 1, 2, 99)
             for event in (0, 1, 2, 3, 99)
             for ticks in (0, 1, 2, 3, 9)]
    collision_pairs = (
        (-2, 20), (-1, 10), (-1, 20), (0, 10), (0, 11), (0, 12),
        (0, 23), (1, -10), (1, -9), (1, 12), (1, 13),
        (2, -20), (2, -9), (2, -8), (2, 13),
        (3, -30), (3, -19), (3, -18), (3, -7),
        (99, -990), (99, -979), (99, -978), (99, -967),
    )
    cases.extend((state, event, ticks)
                 for state, event in collision_pairs for ticks in (0, 2, 3))
    with tempfile.TemporaryDirectory(prefix="ores-fsm-guest-") as scratch:
        d = Path(scratch)
        src, ir, exe = d / "probe.ores", d / "probe.ll", d / "probe"
        for state, event, ticks in cases:
            src.write_text(
                prelude + "\npub fnc main(): int { return step_state("
                + f"{state}, {event}, {ticks}) + 1; }}\n", encoding="utf-8")
            compiled = run([str(compiler), str(src)])
            require(compiled.returncode == 0 and "define i32 @main(" in compiled.stdout,
                    f"guest compile denied {state},{event},{ticks}: {compiled.stderr[:400]}")
            ir.write_text(compiled.stdout, encoding="utf-8")
            linked = run([str(clang), "-O0", "-x", "ir", str(ir), "-o", str(exe)])
            require(linked.returncode == 0, f"LLVM link failed: {linked.stderr[:400]}")
            observed_result = run([str(exe)], timeout=10)
            want = expected(state, event, ticks) + 1
            if oracle is not None:
                independent = oracle_expected(oracle, state, event, ticks)
                if independent is not None:
                    require(independent + 1 == want,
                            f"shared trace oracle disagrees at {state},{event},{ticks}")
                    require(observed_result.returncode == independent + 1,
                            f"guest/shared trace mismatch at {state},{event},{ticks}")
                    oracle_cases += 1
            require(observed_result.returncode == want and not observed_result.stdout
                    and not observed_result.stderr,
                    f"guest mismatch {state},{event},{ticks}: {observed_result.returncode} != {want}")
            observed.append(f"{state}:{event}:{ticks}:{observed_result.returncode}")
        if oracle is not None:
            require(oracle_cases >= 100, "insufficient unsigned shared-trace coverage")
        rejected = run([str(compiler), str(negative)])
        require(rejected.returncode != 0 and not rejected.stdout.strip() and rejected.stderr,
                "duplicate source signature not rejected before IR emission")
    return {
        "kind": "native-guest-fsm-scalar-refinement",
        "fixture_sha256": file_sha256(fixture),
        "compiler_sha256": file_sha256(compiler),
        "clang_sha256": file_sha256(clang),
        "cases": len(cases),
        "shared_trace_oracle_cases": oracle_cases,
        "shared_trace_oracle_sha256": file_sha256(trace_oracle) if oracle else None,
        "observations_sha256": hashlib.sha256("\n".join(observed).encode()).hexdigest(),
        "negative_compiler_rejections": 1,
        "claims": ["scalar-result", "guard-rejection", "unknown-state", "unknown-event"],
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--clang", type=Path, required=True)
    p.add_argument("--source-root", type=Path, required=True)
    p.add_argument("--trace-oracle", type=Path,
                   help="optional exact-source shared FSM trace-v0 Python authority")
    args = p.parse_args()
    try:
        report = verify(args.source_root.resolve(), args.compiler.resolve(),
                        args.clang.resolve(),
                        args.trace_oracle.resolve() if args.trace_oracle else None)
        print(json.dumps(report, sort_keys=True))
        return 0
    except (OSError, AssertionError, ValueError, subprocess.SubprocessError) as exc:
        print("NATIVE_FSM_GUEST_DENIED: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
