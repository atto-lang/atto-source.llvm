#!/usr/bin/env python3
"""LLVM PubSub source gate: .atto/.ores fixture identity and compiler denial.

The native compiler does NOT yet lower ActorGroup/Topic guests. A successful
run here proves *rejection*, not runnable AttoLang PubSub parity.
"""
import argparse
import hashlib
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
CANONICAL_ATTO = "actor-pubsub-formal-suspends-v1.atto"
CANONICAL_GIT_BLOB = "fe10d9e5018a3f030a10bc31586e851198aaa4a3"
SHARE_BANG_DENIAL = "actor-pubsub-reject-unlowered-share-bang-try-publish.atto"
SHARE_BANG_GIT_BLOB = "c562ad380bb283da9ffa26160653c1186ab63462"


def verify_canonical_source(root: pathlib.Path) -> None:
    source = root / "contracts" / "v1" / "fixtures" / CANONICAL_ATTO
    data = source.read_bytes()
    if not data or len(data) > 131072:
        raise AssertionError("canonical .atto source missing or unexpectedly large")
    digest = hashlib.sha1(b"blob " + str(len(data)).encode("ascii")
                          + b"\0" + data).hexdigest()
    if digest != CANONICAL_GIT_BLOB:
        raise AssertionError("canonical .atto source differs from pinned formal PR #579 Git blob")


def verify_fixture_identity(root: pathlib.Path) -> int:
    fixtures = root / "contracts" / "v1" / "fixtures"
    for stem in FIXTURES:
        old = fixtures / (stem + ".ores")
        modern = fixtures / (stem + ".atto")
        if old.read_bytes() != modern.read_bytes():
            raise AssertionError(
                f"{stem}: .atto differs from canonical .ores guest source"
            )
    verify_canonical_source(root)
    # Pin canonical share! negative source independently of the legacy .ores
    # twins; the compiler must refuse it BEFORE generating any guest IR.
    source = fixtures / SHARE_BANG_DENIAL
    data = source.read_bytes()
    if not data or len(data) > 131072:
        raise AssertionError("share! .atto denial source missing or too large")
    digest = hashlib.sha1(b"blob " + str(len(data)).encode("ascii")
                          + b"\0" + data).hexdigest()
    if digest != SHARE_BANG_GIT_BLOB:
        raise AssertionError("share! .atto denial fixture differs from pinned Git blob")
    return len(FIXTURES)


def verify_compiler_rejections(root: pathlib.Path, compiler: str) -> int:
    count = 0
    # This canonical suspends-only .atto source intentionally has no legacy
    # .ores twin. Prove compiler denial, not merely filename identity.
    names = [stem + suffix for stem in FIXTURES
             for suffix in (".ores", ".atto")]
    names.append(CANONICAL_ATTO)
    names.append(SHARE_BANG_DENIAL)
    for name in names:
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
        print(f"PASS SOURCE_ONLY LLVM .atto/.ores PubSub pairs: {paired}/{paired}; pinned canonical suspends .atto blob verified")
        return 0
    if not args.compiler:
        parser.error("--compiler is mandatory for executable negative gate")
    checked = verify_compiler_rejections(root, args.compiler)
    print(f"PASS LLVM pub-sub source rejections: {checked}/{checked} "
          "(unsupported guest lowering; NOT backend parity)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
