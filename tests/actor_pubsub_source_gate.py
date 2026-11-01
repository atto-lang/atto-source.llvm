#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import sys

FIXTURES = (
    "actor-pubsub-v1.ores",
    "actor-pubsub-multigroup-v1.ores",
    "actor-pubsub-reject-payload-type.ores",
    "actor-pubsub-reject-actor-authority.ores",
    "actor-pubsub-reject-private-subscribe.ores",
    "actor-pubsub-reject-mailbox-capability.ores",
    "actor-pubsub-reject-async-capability.ores",
)
DIAGNOSTIC = "native ActorGroup/Topic source lowering unsupported"

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source-root", required=True)
    args = parser.parse_args()

    root = pathlib.Path(args.source_root).resolve()
    for name in FIXTURES:
        source = root / "contracts" / "v1" / "fixtures" / name
        run = subprocess.run(
            [args.compiler, str(source)],
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
            check=False,
        )
        if run.returncode == 0:
            raise AssertionError(f"{name}: unsupported pub/sub source compiled")
        if run.stdout.strip():
            raise AssertionError(f"{name}: rejected source leaked LLVM IR/output")
        if DIAGNOSTIC not in run.stderr:
            raise AssertionError(
                f"{name}: wrong rejection diagnostic: {run.stderr[:800]!r}")
        if run.returncode < 0:
            raise AssertionError(f"{name}: compiler terminated by signal")
    print(f"PASS LLVM pub-sub source gate: {len(FIXTURES)}/{len(FIXTURES)}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
