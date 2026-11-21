"""Fail-closed artifact gate for rejected Atto native compiler invocations.

This is a host test helper, not evidence that any Atto guest executed.
"""
from pathlib import Path


def require_no_rejected_artifacts(case_dir: Path, source: Path, stdout: bytes) -> None:
    """Require exactly the original input file and no compiler stdout.

    The caller must also compare the source SHA-256 before and after compiling.
    Every fixture runs with cwd=case_dir so output files remain observable.
    """
    if not case_dir.is_dir() or case_dir.is_symlink():
        raise AssertionError("invalid compiler test workspace")
    if source.parent != case_dir or not source.is_file() or source.is_symlink():
        raise AssertionError("compiler test source must be a regular local file")
    if stdout.strip():
        raise AssertionError("rejected source emitted stdout (possible partial IR)")
    unexpected = sorted(
        str(path.relative_to(case_dir))
        for path in case_dir.rglob("*")
        if path != source
    )
    if unexpected:
        raise AssertionError("rejected source produced artifacts: " + ", ".join(unexpected))
