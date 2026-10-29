#!/usr/bin/env python3
"""Fail-closed source-local audit of the native actor exact-head CI gate.

This checks path triggers and immutable blob-attestation coverage. It does not
claim to execute the compiler or establish CI-runner availability, and must
never authorize a cross-organization private source transfer.
"""
import pathlib
import re
import sys
import tempfile

if not __debug__:
    raise SystemExit("actor CI dependency audit rejects optimized Python")

INPUT = re.compile(
    r"(?:src/main/cpp|tests)/[A-Za-z0-9_./-]+"
    r"\.(?:cpp|hpp|ores|py|c|h)(?![A-Za-z0-9_.])"
)
JOB = re.compile(r"(?m)^  ([a-z][a-z0-9-]*):\s*$")
TRIGGER = re.compile(
    r"(?m)^      - ((?:src/main/cpp|tests)/[A-Za-z0-9_./*-]+)$"
)
LOCAL_INCLUDE = re.compile(r'(?m)^\s*#include\s+"([^"]+)"')
BLOB_LOOP = re.compile(r"for path in \\\n(.*?); do", re.DOTALL)
REQUIRED_JOBS = ("actor-ledger-contract", "compiled-ores-guest")
MINIMUM_SOURCE_INPUTS = 55


def triggered(path, triggers):
    if path in triggers:
        return True
    return any(
        trigger.endswith("/**") and path.startswith(trigger[:-2])
        for trigger in triggers
    )


def local_header_closure(root, paths):
    """Return recursive repository-owned quoted C/C++ header dependencies.

    Compiler -I src/main/cpp oreslang/ includes resolve from that root;
    relative quotes resolve from the including file. Other unresolved quoted
    includes (for example LLVM SDK headers) are not source-owned files.
    """
    root = root.resolve()
    headers = set()
    pending = list(paths)
    scanned = set()
    while pending:
        item = pending.pop()
        if item in scanned:
            continue
        scanned.add(item)
        if pathlib.PurePosixPath(item).suffix not in {".c", ".cpp", ".h", ".hpp"}:
            continue
        source_file = root / item
        if not source_file.resolve().is_relative_to(root):
            raise AssertionError(f"escaped source path {item}")
        if not source_file.is_file() or source_file.is_symlink():
            raise AssertionError(f"missing or symlink source input: {item}")
        source = source_file.read_text(encoding="utf-8")
        for include in LOCAL_INCLUDE.findall(source):
            if include.startswith("/") or "\\" in include:
                raise AssertionError(f"absolute/invalid quoted include: {include}")
            owned = include.startswith("oreslang/")
            candidate = (root / "src/main/cpp" / include if owned
                         else source_file.parent / include)
            resolved = candidate.resolve()
            if not resolved.is_relative_to(root):
                raise AssertionError(f"quoted include escapes repository: {include}")
            if not candidate.exists():
                if owned:
                    raise AssertionError(f"missing oreslang local include: {include}")
                continue
            if not candidate.is_file() or candidate.is_symlink():
                raise AssertionError(f"non-file or symlink local include: {include}")
            dependency = resolved.relative_to(root).as_posix()
            if not dependency.startswith(("src/main/cpp/", "tests/")):
                raise AssertionError(f"unattestable local include: {dependency}")
            if dependency not in headers:
                headers.add(dependency)
                pending.append(dependency)
    return headers


def audit_selftest():
    directory = "src/main/cpp/oreslang/runtime"
    assert triggered(f"{directory}/added/new.hpp", [f"{directory}/**"])
    assert not triggered(f"{directory}-evil/new.hpp", [f"{directory}/**"])
    assert not triggered(f"{directory}2/new.hpp", [f"{directory}/**"])
    assert triggered("tests/some_test.cpp", ["tests/**"])
    assert not triggered("tests-fake/some_test.cpp", ["tests/**"])
    assert triggered("src/main/cpp/main.cpp", ["src/main/cpp/main.cpp"])
    with tempfile.TemporaryDirectory(prefix="actor-ci-closure-") as temp:
        root = pathlib.Path(temp)
        main = "src/main/cpp/oreslang/demo/entry.cpp"
        local = "src/main/cpp/oreslang/demo/local.hpp"
        nested = "src/main/cpp/oreslang/nested/detail.h"
        for item in (main, local, nested):
            (root / item).parent.mkdir(parents=True, exist_ok=True)
        (root / main).write_text(
            '#include "local.hpp"\n#include "llvm/IR/BasicBlock.h"\n',
            encoding="utf-8",
        )
        (root / local).write_text(
            '#include "../nested/detail.h"\n', encoding="utf-8"
        )
        (root / nested).write_text(
            '#include "../demo/local.hpp"\n', encoding="utf-8"
        )
        dependencies = local_header_closure(root, {main})
        assert dependencies == {local, nested}, dependencies
        assert dependencies - {local} == {nested}, "transitive omission not detected"
        (root / main).write_text(
            '#include "../../../../../../escape.h"\n', encoding="utf-8"
        )
        try:
            local_header_closure(root, {main})
        except AssertionError as exc:
            assert "escapes repository" in str(exc)
        else:
            raise AssertionError("traversal not blocked")
        (root / main).write_text(
            '#include "oreslang/demo/missing.hpp"\n', encoding="utf-8"
        )
        try:
            local_header_closure(root, {main})
        except AssertionError as exc:
            assert "missing oreslang" in str(exc)
        else:
            raise AssertionError("missing mandatory local header not blocked")
    print(
        "ACTOR_CI_HEADER_CLOSURE_SELFTEST_PASS "
        "wildcard=6 relative_cycle=1 transitive_omission=1 "
        "traversal=1 missing_owned=1"
    )


def check():
    root = pathlib.Path(__file__).resolve().parents[1]
    workflow = root / ".github/workflows/actor-execution-ledger-exact-head.yml"
    source = workflow.read_text(encoding="utf-8")
    header, marker, body = source.partition("\njobs:\n")
    assert marker, "missing top-level jobs block"
    trigger_paths = TRIGGER.findall(header)
    trigger_set = set(trigger_paths)
    assert len(trigger_paths) == len(trigger_set), "duplicate PR path trigger"

    boundaries = list(JOB.finditer(body))
    sections = {
        match.group(1): body[match.end(): (
            boundaries[i + 1].start()
            if i + 1 < len(boundaries) else len(body)
        )]
        for i, match in enumerate(boundaries)
    }
    assert set(REQUIRED_JOBS) <= sections.keys(), "missing mandatory jobs"
    all_inputs = set()
    for name in REQUIRED_JOBS:
        section = sections[name]
        loops = BLOB_LOOP.findall(section)
        assert len(loops) == 1, f"{name}: expected one exact-blob loop"
        declared_paths = INPUT.findall(loops[0])
        assert len(declared_paths) == len(set(declared_paths)), (
            f"{name}: duplicate exact-blob attestation path"
        )
        attested = set(declared_paths)
        referenced = set(INPUT.findall(section))
        assert referenced, f"{name}: no source/test dependencies found"
        recursive_headers = local_header_closure(root, referenced)
        required = referenced | recursive_headers
        missing_triggers = {
            item for item in required if not triggered(item, trigger_set)
        }
        missing_blobs = required - attested
        assert not missing_triggers, (
            f"{name}: untriggered dependencies: {sorted(missing_triggers)}"
        )
        assert not missing_blobs, (
            f"{name}: unverified dependencies: {sorted(missing_blobs)}"
        )
        for item in required:
            resolved = (root / item).resolve()
            assert resolved.is_relative_to(root), f"escaped source path {item}"
            assert (root / item).is_file() and not (root / item).is_symlink(), (
                f"missing or symlink source input: {item}"
            )
        all_inputs.update(required)

    assert len(all_inputs) >= MINIMUM_SOURCE_INPUTS, (
        f"source input list unexpectedly shrunk: {len(all_inputs)}"
    )
    for special in (
        ".github/workflows/actor-execution-ledger-exact-head.yml",
        "CMakeLists.txt",
    ):
        assert f"      - {special}\n" in header, (
            f"missing control-file PR trigger: {special}"
        )
        assert special in body, f"missing control-file blob identity: {special}"
    print(
        f"ACTOR_LEDGER_SOURCE_DEPENDENCY_ATTESTATION_PASS "
        f"inputs={len(all_inputs)} jobs={len(REQUIRED_JOBS)}"
    )


if __name__ == "__main__":
    try:
        audit_selftest()
        check()
    except (AssertionError, OSError, UnicodeError) as exc:
        print(f"ACTOR_LEDGER_SOURCE_DEPENDENCY_ATTESTATION_FAIL: {exc}",
              file=sys.stderr)
        sys.exit(1)
