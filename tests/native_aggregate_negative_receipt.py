"""Fail-closed evidence admission for rejected aggregate compiler sources.

A nonzero process exit is insufficient: require the exact diagnostic and no
published compiler product in the isolated case directory. Source-free helper.
"""
from pathlib import Path

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
    if (not result.stdout.startswith("; ModuleID") or
            "define i64 @main(" not in result.stdout or
            "ret i64 42" not in result.stdout):
        raise AssertionError(f"{name}: valid scalar control did not emit expected 42 IR")
    if source.is_symlink() or not source.is_file() or source.read_bytes() != original:
        raise AssertionError(f"{name}: compiler mutated or replaced scalar control")
    leftovers = [p.name for p in source.parent.iterdir() if p != source]
    if leftovers:
        raise AssertionError(f"{name}: scalar control left artifacts: {leftovers}")
