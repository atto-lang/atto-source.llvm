#!/usr/bin/env python3
"""LLVM PubSub source gate: .atto/.ores fixture identity and compiler denial.

The native compiler does NOT yet lower ActorGroup/Topic guests. A successful
run here proves *rejection*, not runnable AttoLang PubSub parity.
"""
import argparse
import pathlib
import subprocess
import sys

FIXTURES = (
    "actor-pubsub-v1",
    "actor-pubsub-multigroup-v1",
    "actor-pubsub-reject-payload-type",
    "actor-pubsub-reject-actor-authority",
    "actor-pubsub-reject-private-subscribe",
    "actor-pubsub-reject-mailbox-capability",
    "actor-pubsub-reject-async-capability",
)
DIAGNOSTIC = "native ActorGroup/Topic source lowering unsupported"


def verify_fixture_identity(root: pathlib.Path) -> int:
    fixtures = root / "contracts" / "v1" / "fixtures"
    for stem in FIXTURES:
        old = fixtures / (stem + ".ores")
        modern = fixtures / (stem + ".atto")
        if old.read_bytes() != modern.read_bytes():
            raise AssertionError(
                f"{stem}: .atto differs from canonical .ores guest source"
            )
    return len(FIXTURES)


def verify_compiler_rejections(root: pathlib.Path, compiler: str) -> int:
    count = 0
    for stem in FIXTURES:
        for suffix in (".ores", ".atto"):
            name = stem + suffix
            source = root / "contracts" / "v1" / "fixtures" / name
            run = subprocess.run(
                [compiler, str(source)],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=20,
                check=False,
            )
            # A zero exit, signal crash or any emitted stdout disqualifies a
            # fail-closed compiler-negative test.
            if run.returncode <= 0:
                raise AssertionError(
                    f"{name}: unsupported source accepted or compiler crashed "
                    f"(status {run.returncode})"
                )
            if run.stdout:
                raise AssertionError(
                    f"{name}: rejected guest produced stdout/IR "
                    f"({len(run.stdout)} bytes)"
                )
            if DIAGNOSTIC not in run.stderr:
                raise AssertionError(
                    f"{name}: not an ActorGroup/Topic parser rejection: "
                    f"{run.stderr[:800]!r}"
                )
            count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler")
    parser.add_argument("--source-root", required=True)
    parser.add_argument(
        "--verify-fixtures-only",
        action="store_true",
        help="check byte identity only (non-promoting, no compiler executed)",
    )
    args = parser.parse_args()

    root = pathlib.Path(args.source_root).resolve()
    paired = verify_fixture_identity(root)
    if args.verify_fixtures_only:
        print(f"PASS SOURCE_ONLY LLVM .atto/.ores PubSub pairs: {paired}/{paired}")
        return 0
    if not args.compiler:
        parser.error("--compiler is mandatory for executable negative gate")
    checked = verify_compiler_rejections(root, args.compiler)
    print(f"PASS LLVM pub-sub source rejections: {checked}/{checked} "
          "(unsupported guest lowering; NOT backend parity)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
