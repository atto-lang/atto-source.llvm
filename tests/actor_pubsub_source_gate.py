#!/usr/bin/env python3
"""LLVM PubSub source gate: .atto/.ores fixture identity and compiler denial.

The native compiler does NOT yet lower ActorGroup/Topic guests. A successful
run here proves *rejection*, not runnable AttoLang PubSub parity.
"""
import argparse
import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile

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
# Exact positive .atto guest fixture from Java compiler PR #1458. LLVM still
# must reject it until real Topic<T>/Result lowering exists.
JAVA_TYPED_TRY_PUBLISH = "actor-pubsub-try-publish-v1.atto"
JAVA_TYPED_TRY_GIT_BLOB = "41fda4efc19823807b56545a31230b6182de0ee1"


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
    # Java's typed-positive guest fixture must be byte-identical before
    # the LLVM compiler-negative check. This never certifies native lowering.
    java_source = fixtures / JAVA_TYPED_TRY_PUBLISH
    java_data = java_source.read_bytes()
    if not java_data or len(java_data) > 131072:
        raise AssertionError("Java typed try_publish fixture missing/oversized")
    java_blob = hashlib.sha1(
        b"blob " + str(len(java_data)).encode("ascii") + b"\0" + java_data
    ).hexdigest()
    if java_blob != JAVA_TYPED_TRY_GIT_BLOB:
        raise AssertionError("Java typed try_publish fixture Git blob differs")
    return len(FIXTURES)



def run_one_compiler_rejection(
        root: pathlib.Path, compiler_argv: list[str], name: str) -> None:
    """Execute an actual native compiler negative inside a private scratch dir.

    Do not accept a typed diagnostic alone if the compiler also staged an IR
    file, rewrote the source, or generated any other build artifact. The CLI
    may write beside its input file or relative to cwd; both are isolated.
    This proves only compiler rejection, never positive guest execution.
    """
    original = root / "contracts" / "v1" / "fixtures" / name
    if original.is_symlink() or not original.is_file():
        raise AssertionError(f"{name}: source must be a regular non-symlink file")
    payload = original.read_bytes()
    if not payload or len(payload) > 131072:
        raise AssertionError(f"{name}: negative fixture missing or oversized")
    with tempfile.TemporaryDirectory(prefix="atto-llvm-pubsub-negative-") as scratch:
        workspace = pathlib.Path(scratch)
        snapshot = workspace / name
        snapshot.write_bytes(payload)
        snapshot.chmod(0o400)
        run = subprocess.run(
            [*compiler_argv, str(snapshot)],
            cwd=workspace,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
            check=False,
        )
        if run.returncode <= 0:
            raise AssertionError(
                f"{name}: unsupported source accepted or compiler crashed "
                f"(status {run.returncode})"
            )
        if run.stdout:
            raise AssertionError(
                f"{name}: rejected guest produced stdout/IR "
                f"({len(run.stdout)} characters)"
            )
        if DIAGNOSTIC not in run.stderr:
            raise AssertionError(
                f"{name}: not an ActorGroup/Topic parser rejection: "
                f"{run.stderr[:800]!r}"
            )
        # An error code and diagnostic are not proof of clean rejection if
        # generated artifacts remain. Snapshot fidelity is checked as well.
        if snapshot.is_symlink() or not snapshot.is_file() \
                or snapshot.read_bytes() != payload:
            raise AssertionError(f"{name}: compiler modified negative source")
        unexpected = [p.name for p in workspace.iterdir() if p != snapshot]
        if unexpected:
            raise AssertionError(
                f"{name}: compiler emitted unexpected files: {unexpected}"
            )


def verify_compiler_rejections(root: pathlib.Path, compiler: str) -> int:
    count = 0
    # CTest passes $<TARGET_FILE:oreslang-llvmc> (absolute). Relative binary
    # arguments remain relative to the fixture repository, not the sandbox.
    compiler_path = pathlib.Path(compiler)
    if not compiler_path.is_absolute():
        compiler_path = root / compiler_path
    compiler_command = [str(compiler_path.resolve())]
    names = [stem + suffix for stem in FIXTURES
             for suffix in (".ores", ".atto")]
    names.extend((CANONICAL_ATTO, SHARE_BANG_DENIAL, JAVA_TYPED_TRY_PUBLISH))
    for name in names:
        run_one_compiler_rejection(root, compiler_command, name)
        count += 1
    return count


def self_test_compiler_negative_boundary() -> int:
    """Small source-free adversarial proof of the checker, NOT LLVM execution."""
    with tempfile.TemporaryDirectory(prefix="atto-pubsub-gate-selftest-") as scratch:
        root = pathlib.Path(scratch)
        fixtures = root / "contracts" / "v1" / "fixtures"
        fixtures.mkdir(parents=True)
        (fixtures / "probe.atto").write_text(
            "pub fnc main(): void { val result = topic.try_publish(share! x); }\n",
            encoding="utf-8",
        )
        stub = root / "fake_compiler.py"
        stub.write_text(
            "import pathlib,sys\n"
            "kind=sys.argv[1]\n"
            "if kind=='emits': pathlib.Path('guest.ll').write_text('IR')\n"
            "if kind=='mutates': pathlib.Path(sys.argv[2]).chmod(0o600);"
            " pathlib.Path(sys.argv[2]).write_text('modified')\n"
            "if kind=='stdout': print('fake IR')\n"
            f"print({DIAGNOSTIC!r},file=sys.stderr)\n"
            "sys.exit(0 if kind=='accepted' else 2)\n",
            encoding="utf-8",
        )
        for kind in ("emits", "mutates", "stdout", "accepted"):
            try:
                run_one_compiler_rejection(
                    root, [sys.executable, str(stub), kind], "probe.atto"
                )
            except AssertionError:
                pass
            else:
                raise AssertionError(
                    f"source-free checker incorrectly accepted {kind} case"
                )
        run_one_compiler_rejection(
            root, [sys.executable, str(stub), "clean"], "probe.atto"
        )
    return 5


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler")
    parser.add_argument("--self-test", action="store_true",
                        help="adversarial source-free checker tests; no LLVM guest")
    parser.add_argument("--source-root")
    parser.add_argument(
        "--verify-fixtures-only",
        action="store_true",
        help="check byte identity only (non-promoting, no compiler executed)",
    )
    args = parser.parse_args()
    if args.self_test:
        count = self_test_compiler_negative_boundary()
        print(f"PASS SOURCE_ONLY LLVM negative-checker selftest: {count}/{count}")
        return 0
    if not args.source_root:
        parser.error("--source-root is required unless --self-test is selected")

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
