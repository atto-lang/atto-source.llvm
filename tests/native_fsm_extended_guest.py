#!/usr/bin/env python3
"""Native Oreslang *scalar projection* differential tests against trace-v0.

This compiles/links/executes guest source for state, version, ticks and effect
ID separately. It is NOT a transactional store or effect outbox test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import native_fsm_scalar_guest as base

FIELDS = ("state", "version", "ticks", "effect")
VERSIONS = (0, 127, 254, -1)  # -1 is the signed native representation of u32 max.
STATES = (0, 1, 2, 99)
EVENTS = (0, 1, 2, 3, 99)
TICKS = (0, 1, 2, 3, 9)


def expected(oracle, state, event, ticks, version):
    unsigned_version = oracle.LIMIT if version == -1 else version
    before = oracle.snap(state, unsigned_version, ticks)
    accepted, reason, after, effects = oracle.step(before, event)
    base.require(type(accepted) is bool and type(reason) is str
                 and type(after) is dict and type(effects) is list,
                 "bad independent oracle projection")
    base.require(oracle.validate({
        "contract": oracle.CONTRACT, "machine": "flat-fixture-v0",
        "entity_count": 1,
        "transitions": [{
            "seq": 0, "entity": 0, "event": event,
            "accepted": accepted, "reason": reason,
            "before": before, "after": after, "effects": effects,
        }],
    }) is True, "shared oracle does not validate its own transition")
    result = (
        after["state"], after["version"] % 256,
        after["ticks"], effects[0] if effects else 0,
    )
    base.require(all(type(x) is int and 0 <= x <= 255 for x in result),
                 "projected value cannot be independently observed as exit status")
    return result, accepted


def verify(root, compiler, clang, oracle_path):
    base.require(compiler.is_file() and clang.is_file(),
                 "missing native Oreslang compiler or clang")
    base.require(compiler.stat().st_mode & 0o111 and clang.stat().st_mode & 0o111,
                 "nonexecutable native compiler or clang")
    fixture = root / "tests/fixtures/fsm-scalar-reducer.ores"
    base.require(fixture.is_file() and fixture.resolve().is_relative_to(root),
                 "missing or escaping guest FSM source")
    prelude = fixture.read_text(encoding="utf-8")
    base.require("pub fnc main(" not in prelude and len(prelude) <= 4096,
                 "oversized or unsafe native FSM fixture")
    for name in FIELDS:
        base.require(f"fnc project_{name}(" in prelude, "missing projector: " + name)
    oracle = base.load_trace_oracle(oracle_path)
    evidence = hashlib.sha256()
    accepted_count = 0
    cases = 0
    with tempfile.TemporaryDirectory(prefix="ores-native-trace-") as directory:
        tmp = Path(directory)
        source, llvm, binary = (tmp / "trace.ores", tmp / "trace.ll", tmp / "trace")
        for version in VERSIONS:
            for state in STATES:
                for event in EVENTS:
                    for ticks in TICKS:
                        expected_parts, accepted = expected(
                            oracle, state, event, ticks, version)
                        accepted_count += int(accepted)
                        for field, outcome in zip(FIELDS, expected_parts):
                            source.write_text(prelude +
                                "\npub fnc main(): int { return project_" + field
                                + f"({state}, {event}, {ticks}, {version}); }}\n",
                                encoding="utf-8")
                            compile_result = base.run([str(compiler), str(source)])
                            base.require(compile_result.returncode == 0
                                and "define i32 @main(" in compile_result.stdout,
                                "guest projector compile failed: "
                                + field + " " + compile_result.stderr[:400])
                            llvm.write_text(compile_result.stdout, encoding="utf-8")
                            link_result = base.run([str(clang), "-O0", "-x", "ir",
                                                    str(llvm), "-o", str(binary)])
                            base.require(link_result.returncode == 0,
                                         "guest projector link failed: "
                                         + link_result.stderr[:400])
                            ran = base.run([str(binary)], timeout=10)
                            base.require(ran.returncode == outcome and not ran.stdout
                                         and not ran.stderr,
                                         f"guest/oracle {field} mismatch "
                                         f"{state},{event},{ticks},{version}: "
                                         f"{ran.returncode} != {outcome}")
                            evidence.update(
                                f"{state}:{event}:{ticks}:{version}:{field}:{outcome}\n"
                                .encode("ascii"))
                        cases += 1
    return {
        "kind": "native-guest-bounded-trace-projections",
        "cases": cases, "projections": cases * len(FIELDS),
        "accepted_cases": accepted_count,
        "shared_trace_oracle_sha256": base.file_sha256(oracle_path),
        "fixture_sha256": base.file_sha256(fixture),
        "compiler_sha256": base.file_sha256(compiler),
        "observations_sha256": evidence.hexdigest(),
        "limitations": ["exit-status-u8-projections", "u32max-sentinel",
                        "no-atomic-version-commit", "no-effect-outbox",
                        "no-cross-entity-native-state", "not-java-guest-parity"],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--trace-oracle", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = verify(args.source_root.resolve(), args.compiler.resolve(),
                        args.clang.resolve(), args.trace_oracle.resolve())
        print(json.dumps(report, sort_keys=True))
        return 0
    except (ValueError, OSError, AssertionError, subprocess.SubprocessError) as exc:
        print("NATIVE_FSM_TRACE_DENIED: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
