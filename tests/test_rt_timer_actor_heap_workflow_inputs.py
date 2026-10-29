#!/usr/bin/env python3
"""Fail closed if focused RT exact-head CI stops covering a source input."""
import pathlib
import re
import sys

if not __debug__:
    raise SystemExit("RT timer CI dependency audit rejects optimized Python")

INPUT = re.compile(
    r"(?:src/main/cpp|tests)/[A-Za-z0-9_./-]+"
    r"\.(?:cpp|hpp|py|c|h)(?![A-Za-z0-9_.])"
)
TRIGGER = re.compile(
    r"(?m)^      - ((?:src/main/cpp|tests)/[A-Za-z0-9_./*-]+)$"
)
LOCAL_INCLUDE = re.compile(r'(?m)^\s*#include\s+"([^"]+)"')
BLOB_LOOP = re.compile(r"for path in\s+(.*?); do", re.DOTALL)
JOB = "rt-timer-actor-heap"


def triggered(path, triggers):
    if path in triggers:
        return True
    return any(
        item.endswith("/**") and path.startswith(item[:-3])
        for item in triggers
    )


def local_header_closure(root, paths):
    headers = set()
    pending = list(paths)
    scanned = set()
    while pending:
        item = pending.pop()
        if item in scanned:
            continue
        scanned.add(item)
        if pathlib.Path(item).suffix not in {".c", ".cpp", ".h", ".hpp"}:
            continue
        source = (root / item).read_text(encoding="utf-8")
        for include in LOCAL_INCLUDE.findall(source):
            if not include.startswith("oreslang/"):
                continue
            dependency = "src/main/cpp/" + include
            if dependency not in headers:
                headers.add(dependency)
                pending.append(dependency)
    return headers


def check():
    root = pathlib.Path(__file__).resolve().parents[1]
    workflow = root / ".github/workflows/rt-timer-actor-heap-exact-head.yml"
    source = workflow.read_text(encoding="utf-8")
    header, marker, body = source.partition("\njobs:\n")
    assert marker, "missing top-level jobs block"
    trigger_paths = TRIGGER.findall(header)
    trigger_set = set(trigger_paths)
    assert len(trigger_paths) == len(trigger_set), "duplicate PR path trigger"

    job_marker = f"  {JOB}:\n"
    assert job_marker in body, "missing focused RT job"
    section = body.split(job_marker, 1)[1]
    loops = BLOB_LOOP.findall(section)
    assert len(loops) == 1, "expected one exact-blob loop"
    loop_text = loops[0]
    declared = INPUT.findall(loop_text)
    assert len(declared) == len(set(declared)), "duplicate exact-blob input"
    attested = set(declared)

    referenced = set(INPUT.findall(section))
    assert referenced, "no focused RT source/test dependencies found"
    required = referenced | local_header_closure(root, referenced)
    missing_triggers = {
        item for item in required if not triggered(item, trigger_set)
    }
    missing_blobs = required - attested
    assert not missing_triggers, (
        f"untriggered dependencies: {sorted(missing_triggers)}"
    )
    assert not missing_blobs, (
        f"unverified dependencies: {sorted(missing_blobs)}"
    )
    for item in required:
        resolved = (root / item).resolve()
        assert resolved.is_relative_to(root), f"escaped source path {item}"
        assert (root / item).is_file() and not (root / item).is_symlink(), (
            f"missing or symlink source input: {item}"
        )

    for special in (
        "CMakeLists.txt",
        ".github/workflows/rt-timer-actor-heap-exact-head.yml",
    ):
        assert f"      - {special}\n" in header, (
            f"missing control-file PR trigger: {special}"
        )
        assert special in loop_text, f"missing control-file blob identity: {special}"

    print(
        "RT_TIMER_ACTOR_HEAP_SOURCE_DEPENDENCY_ATTESTATION_PASS "
        f"inputs={len(required)}"
    )


if __name__ == "__main__":
    try:
        check()
    except (AssertionError, OSError, UnicodeError) as exc:
        print(
            f"RT_TIMER_ACTOR_HEAP_SOURCE_DEPENDENCY_ATTESTATION_FAIL: {exc}",
            file=sys.stderr,
        )
        sys.exit(1)
