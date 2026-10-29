"""Fail-closed evidence admission for rejected aggregate compiler sources.

A nonzero process exit is insufficient: require the exact diagnostic and no
published compiler product in the isolated case directory. Source-free helper.
"""
from pathlib import Path
import hashlib
import re


def git_blob_sha1(contents: bytes) -> str:
    """Hash the exact source bytes as a Git blob, not an ordinary SHA-1."""
    return hashlib.sha1(b"blob " + str(len(contents)).encode("ascii") +
                        bytes([0]) + contents).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def result_digest(output: str) -> str:
    return hashlib.sha256(output.encode("utf-8")).hexdigest()


def complete_negative_receipt(compiler_sha256: str, control: dict,
                              cases: list[dict], expected: list[str]) -> dict:
    """Construct a negative-only receipt, never a guest/CI provenance claim.

    Call this ONLY after verifying every subprocess and its clean workspace.
    Reject incomplete, reordered, duplicated, or unexpected fixture records.
    A trusted CI job must independently bind exact Git source/tree and toolchain
    identities; caller-supplied hashes are not such an attestation.
    """
    hexdigest = re.compile(r"[a-f0-9]{64}", re.ASCII)
    blob = re.compile(r"[a-f0-9]{40}", re.ASCII)
    if not hexdigest.fullmatch(compiler_sha256):
        raise AssertionError("receipt: invalid compiler SHA-256")
    if not expected or len(expected) != len(set(expected)):
        raise AssertionError("receipt: duplicate/empty fixture registry")
    if len(cases) != len(expected) or [case.get("id") for case in cases] != expected:
        raise AssertionError("receipt: incomplete/reordered fixture executions")
    if control.get("id") != "scalar-positive" or control.get("returnCode") != 0:
        raise AssertionError("receipt: missing successful scalar compiler control")
    if control.get("verified") is not True or not blob.fullmatch(
            str(control.get("sourceGitBlobSha1", ""))) or not hexdigest.fullmatch(
            str(control.get("stdoutSha256", ""))):
        raise AssertionError("receipt: invalid scalar control digest")
    for case in cases:
        if (case.get("verified") is not True or type(case.get("returnCode")) is not int or
                case["returnCode"] <= 0 or
                not re.fullmatch(r"[A-Z][A-Z0-9_]+", str(case.get("diagnostic", ""))) or
                not blob.fullmatch(str(case.get("sourceGitBlobSha1", ""))) or
                not hexdigest.fullmatch(str(case.get("stderrSha256", "")))):
            raise AssertionError("receipt: unverified compiler case " + str(case.get("id")))
    return {
        "schemaVersion": 1,
        "scope": "aggregate-kind-native-compiler-negative-only",
        "guestAggregateLoweringVerified": False,
        "exactSourceProvenanceVerified": False,
        "compilerSha256": compiler_sha256,
        "scalarControl": control,
        "expectedNegativeCaseCount": len(expected),
        "executedNegativeCaseCount": len(cases),
        "cases": cases,
    }

IR_MARKERS = ("; ModuleID", "source_filename =", "target triple =", "define ")


def verify_negative_case(name: str, source: Path, original: bytes,
                         diagnostic: str, result) -> None:
    if type(result.returncode) is not int or result.returncode <= 0:
        raise AssertionError(f"{name}: compiler did not fail")
    if diagnostic not in result.stderr:
        raise AssertionError(f"{name}: expected diagnostic {diagnostic!r} in stderr; "
                             f"got {result.stderr[:1200]!r}")
    if result.stdout.strip():
        raise AssertionError(f"{name}: rejected source produced stdout, possibly LLVM IR")
    if any(marker in result.stderr for marker in IR_MARKERS):
        raise AssertionError(f"{name}: rejected source leaked LLVM IR in stderr")
    if source.is_symlink() or not source.is_file() or source.read_bytes() != original:
        raise AssertionError(f"{name}: compiler mutated or replaced original source")
    leftovers = [p.name for p in source.parent.iterdir() if p != source]
    if leftovers:
        raise AssertionError(f"{name}: rejected source left artifacts: {leftovers}")


def verify_scalar_positive_control(name: str, source: Path, original: bytes,
                                   result) -> None:
    """Require actual successful scalar IR before trusting aggregate rejections.

    This is a compiler liveness control, NOT proof of aggregate lowering or
    native executable behavior. A compiler that simply rejects every input
    must not pass the negative suite.
    """
    if type(result.returncode) is not int or result.returncode != 0:
        raise AssertionError(f"{name}: valid scalar control did not compile")
    if result.stderr.strip():
        raise AssertionError(f"{name}: valid scalar control emitted stderr: "
                             f"{result.stderr[:1200]!r}")
    guest_body = re.search(
        r"define\s+i64\s+@__oreslang_guest_main\([^)]*\)\s*\{([^}]*)\}",
        result.stdout, re.DOTALL)
    if (not result.stdout.startswith("; ModuleID") or
            guest_body is None or
            "define i32 @main(" not in result.stdout or
            re.search(r"\bret\s+i64\s+42\b", guest_body.group(1)) is None):
        raise AssertionError(f"{name}: valid scalar control did not emit expected 42 IR")
    if source.is_symlink() or not source.is_file() or source.read_bytes() != original:
        raise AssertionError(f"{name}: compiler mutated or replaced scalar control")
    leftovers = [p.name for p in source.parent.iterdir() if p != source]
    if leftovers:
        raise AssertionError(f"{name}: scalar control left artifacts: {leftovers}")


def verify_isolated_workspace(root: Path, expected: dict) -> None:
    """Reject artifacts that escaped a case directory into its test-root siblings.

    The per-case checks alone are insufficient: a misbehaving compiler could
    write ../output.ll and leave its own case directory clean.
    """
    if root.is_symlink() or not root.is_dir():
        raise AssertionError("aggregate workspace root replaced")
    actual = {entry.name for entry in root.iterdir()}
    if actual != set(expected):
        raise AssertionError(f"aggregate workspace contains unexpected entries: "
                             f"{sorted(actual ^ set(expected))}")
    for case_name, (source, original) in expected.items():
        directory = root / case_name
        if (directory.is_symlink() or not directory.is_dir() or
                source.parent != directory or source.is_symlink() or
                not source.is_file() or source.read_bytes() != original):
            raise AssertionError(f"{case_name}: aggregate workspace source changed")
        children = list(directory.iterdir())
        if len(children) != 1 or children[0] != source:
            raise AssertionError(f"{case_name}: aggregate workspace contains artifacts")
