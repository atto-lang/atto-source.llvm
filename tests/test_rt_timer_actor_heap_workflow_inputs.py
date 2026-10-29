#!/usr/bin/env python3
"""Fail closed if focused RT exact-head CI stops covering a source input."""
import pathlib
import re
import sys
import tempfile

if not __debug__:
    raise SystemExit("RT timer CI dependency audit rejects optimized Python")

INPUT = re.compile(
    r"(?:src/main/cpp|tests)/[A-Za-z0-9_./-]+"
    r"\.(?:cpp|hpp|py|c|h)(?![A-Za-z0-9_.])"
)
TRIGGER = re.compile(
    r"(?m)^      - ((?:src/main/cpp|tests)/[A-Za-z0-9_./*-]+)$"
)
LOCAL_INCLUDE = re.compile(r'(?m)^[ \t]*#[ \t]*include[ \t]+"([^"\r\n]+)"')
BLOB_LOOP = re.compile(r"for path in\s+(.*?); do", re.DOTALL)
JOB = "rt-timer-actor-heap"


def triggered(path, triggers):
    if path in triggers:
        return True
    return any(
        item.endswith("/**") and path.startswith(item[:-2])
        for item in triggers
    )


def reject_symlink_ancestors(root, path):
    """Reject source-bearing paths crossing any intermediate symlink.

    Git blob attestation on the target file alone cannot attest an aliasing
    directory symlink. This also applies when the target stays within root.
    Direct leaf symlinks are rejected separately by the caller.
    """
    try:
        parts = path.relative_to(root).parts
    except ValueError as exc:
        raise AssertionError(f"source path outside repository: {path}") from exc
    cursor = root
    for component in parts[:-1]:
        if component == "..":
            cursor = cursor.parent
        elif component != ".":
            cursor = cursor / component
        if not cursor.is_relative_to(root):
            raise AssertionError(f"source path traverses repository root: {path}")
        if cursor.is_symlink():
            raise AssertionError(f"symlink path ancestor: {path}")


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
        reject_symlink_ancestors(root, source_file)
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
            reject_symlink_ancestors(root, candidate)
            if candidate.is_symlink():
                raise AssertionError(f"symlink local include: {include}")
            if not candidate.exists():
                if include.startswith("llvm/"):
                    continue  # SDK headers are not private repository inputs.
                if owned:
                    raise AssertionError(
                        f"missing oreslang local include: {include}"
                    )
                raise AssertionError(
                    f"missing repository-local quoted include: {include}"
                )
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
    assert not triggered("tests/new/helper.hpp", ["tests/test_actor_ledger_workflow_inputs.py"])
    assert triggered("src/main/cpp/main.cpp", ["src/main/cpp/main.cpp"])
    with tempfile.TemporaryDirectory(prefix="rt-timer-ci-closure-") as temp:
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
        # Standard C/C++ preprocessor whitespace must not bypass exact-blob
        # dependency closure and transitive omission detection.
        (root / main).write_text(
            '# include "local.hpp"\n#\tinclude "../nested/detail.h"\n',
            encoding="utf-8",
        )
        spaced = local_header_closure(root, {main})
        assert spaced == {local, nested}, "spaced preprocessor include skipped"
        assert spaced - {local} == {nested}, "spaced include omission not detected"
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
        (root / main).write_text(
            '#include "missing-local.hpp"\n', encoding="utf-8"
        )
        try:
            local_header_closure(root, {main})
        except AssertionError as exc:
            assert "missing repository-local quoted include" in str(exc)
        else:
            raise AssertionError("missing relative local header not blocked")
        (root / main).write_text(
            '#include "dangling.hpp"\n', encoding="utf-8"
        )
        (root / "src/main/cpp/oreslang/demo/dangling.hpp").symlink_to("absent.hpp")
        try:
            local_header_closure(root, {main})
        except AssertionError as exc:
            assert "symlink local include" in str(exc)
        else:
            raise AssertionError("dangling local symlink not blocked")
        # A symlinked directory aliases a regular source file and would
        # bypass an is_symlink() check on that file alone.
        alias = root / "src/main/cpp/oreslang/demo-alias"
        alias.symlink_to("demo", target_is_directory=True)
        try:
            local_header_closure(
                root, {"src/main/cpp/oreslang/demo-alias/entry.cpp"}
            )
        except AssertionError as exc:
            assert "symlink path ancestor" in str(exc)
        else:
            raise AssertionError("symlinked source directory not blocked")

        # Likewise reject relative includes through an intermediate alias,
        # even if the symlink points to a file inside the same repository.
        shadow = root / "src/main/cpp/oreslang/demo/shadow"
        shadow.symlink_to("../nested", target_is_directory=True)
        (root / main).write_text(
            '#include "shadow/detail.h"\n', encoding="utf-8"
        )
        try:
            local_header_closure(root, {main})
        except AssertionError as exc:
            assert "symlink path ancestor" in str(exc)
        else:
            raise AssertionError("symlinked local include directory not blocked")
    print(
        "RT_TIMER_CI_HEADER_CLOSURE_SELFTEST_PASS "
        "wildcard=6 relative_cycle=1 transitive_omission=1 "
        "traversal=1 missing_owned=1 symlink_ancestors=2"
    )


def check():
    root = pathlib.Path(__file__).resolve().parents[1]
    workflow = root / ".github/workflows/rt-timer-actor-heap-exact-head.yml"
    source = workflow.read_text(encoding="utf-8")
    header, marker, body = source.partition("\njobs:\n")
    assert marker, "missing top-level jobs block"
    trigger_paths = TRIGGER.findall(header)
    trigger_set = set(trigger_paths)
    assert len(trigger_paths) == len(trigger_set), "duplicate PR path trigger"

    # Broad trigger coverage must survive adding a new quoted runtime header
    # or test helper that is not yet in the fixed exact-blob manifest. Its
    # introducing PR will run this audit and fail closed until attested.
    for wildcard, new_input, sibling in (
        ("src/main/cpp/oreslang/runtime/**",
         "src/main/cpp/oreslang/runtime/NewTimerDependency.hpp",
         "src/main/cpp/oreslang/runtime-other/Unrelated.hpp"),
        ("tests/**",
         "tests/new_rt_timer_helper.hpp",
         "tests-other/unrelated.hpp"),
    ):
        assert wildcard in trigger_set, f"missing broad PR trigger: {wildcard}"
        assert triggered(new_input, trigger_set), (
            f"new dependency cannot trigger focused CI: {new_input}"
        )
        assert not triggered(sibling, {wildcard}), (
            f"wildcard escapes directory boundary: {wildcard}"
        )

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
    # Blob identity alone does not reject untracked or modified
    # non-manifest inputs. Require a clean checkout before builds.
    identity_name = "Verify exact commit and relevant Git blobs"
    identity_marker = f"      - name: {identity_name}\n"
    assert section.count(identity_marker) == 1, "missing identity gate"
    identity_step = section.split(identity_marker, 1)[1].split(
        "\n      - name:", 1
    )[0]
    assert re.search(r"(?m)^\s*set -euo pipefail\s*$", identity_step), (
        "identity gate is not fail-fast"
    )
    assert re.search(
        r'(?m)^\s*test -z "\$\(git status --porcelain '
        r'--untracked-files=all\)"\s*$', identity_step
    ), "missing clean checkout guard"

    # Keep the native test matrix executable, not merely listed in the blob
    # manifest. In particular, deleting a timer or heap command must fail the
    # same self-audit that already protects the authenticated ledger fixture.
    build_matrix = {
        "GCC strict timer and actor-heap execution": (
            ("tests/native_realtime_timer_queue_test.cpp", "timer-gcc", None),
            ("tests/native_rt_timer_actor_heap_integration_test.cpp",
             "integration-gcc", "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
            ("tests/native_rt_timer_actor_ledger_integration_test.cpp",
             "ledger-gcc", "src/main/cpp/oreslang/runtime/NativeActorScheduler.c"),
        ),
        "Clang strict timer and actor-heap execution": (
            ("tests/native_realtime_timer_queue_test.cpp", "timer-clang", None),
            ("tests/native_rt_timer_actor_heap_integration_test.cpp",
             "integration-clang", "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
            ("tests/native_rt_timer_actor_ledger_integration_test.cpp",
             "ledger-clang", "src/main/cpp/oreslang/runtime/NativeActorScheduler.c"),
        ),
        "Clang ASan and UBSan actor-heap execution": (
            ("tests/native_realtime_timer_queue_test.cpp",
             "timer-san", None),
            ("tests/native_rt_timer_actor_heap_integration_test.cpp",
             "integration-san", "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
            ("tests/native_rt_timer_actor_ledger_integration_test.cpp",
             "ledger-san", "src/main/cpp/oreslang/runtime/NativeActorScheduler.c"),
        ),
    }
    for step_name, expectations in build_matrix.items():
        step_marker = f"      - name: {step_name}\n"
        assert section.count(step_marker) == 1, (
            f"missing or duplicate execution step: {step_name}"
        )
        step = section.split(step_marker, 1)[1].split("\n      - name:", 1)[0]
        assert re.search(r"(?m)^\s*set -euo pipefail\s*$", step), (
            f"{step_name}: native step is not fail-fast"
        )
        assert "continue-on-error: true" not in step, (
            f"{step_name}: failures are explicitly ignored"
        )
        assert "|| true" not in step, (
            f"{step_name}: failures may be masked"
        )
        if step_name == "Clang ASan and UBSan actor-heap execution":
            assert step.count("-fsanitize=address,undefined") >= 4, (
                "sanitizer instrumentation missing from build/link commands"
            )
            assert step.count("-fno-omit-frame-pointer") >= 4, (
                "sanitizer frame-pointer instrumentation missing"
            )
            assert step.count("ASAN_OPTIONS=") >= 2, (
                "ASan runtime enforcement missing"
            )
            assert step.count("UBSAN_OPTIONS=") >= 2, (
                "UBSan runtime enforcement missing"
            )
        for fixture, binary, dependency in expectations:
            assert fixture in step, f"{step_name}: missing {fixture}"
            if dependency:
                assert dependency in step, (
                    f"{step_name}: missing linked source {dependency}"
                )
            binary_path = f"build/rt-timer-actor-heap/{binary}"
            assert re.search(
                r"(?m)(?:^|\s)-o\s+" + re.escape(binary_path) + r"(?:\s|$)",
                step,
            ), f"{step_name}: missing {binary} build output"
            assert re.search(
                r"(?m)^\s*(?:(?:ASAN_OPTIONS|UBSAN_OPTIONS)=[^\s]+\s+)*"\n                + re.escape(binary_path) + r"\s*$", step
            ), f"{step_name}: {binary} never executed"

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for test_name, fixture in (
        ("native_realtime_timer_queue_contract",
         "tests/native_realtime_timer_queue_test.cpp"),
        ("native_rt_timer_actor_heap_loan_lifecycle",
         "tests/native_rt_timer_actor_heap_integration_test.cpp"),
        ("native_rt_timer_actor_ledger_start_authority",
         "tests/native_rt_timer_actor_ledger_integration_test.cpp"),
    ):
        assert f"add_test(NAME {test_name}" in cmake, (
            f"CMake registration missing: {test_name}"
        )
        assert fixture in cmake, f"CMake fixture missing: {fixture}"

    for item in required:
        resolved = (root / item).resolve()
        assert resolved.is_relative_to(root), f"escaped source path {item}"
        reject_symlink_ancestors(root.resolve(), root / item)
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
        audit_selftest()
        check()
    except (AssertionError, OSError, UnicodeError) as exc:
        print(
            f"RT_TIMER_ACTOR_HEAP_SOURCE_DEPENDENCY_ATTESTATION_FAIL: {exc}",
            file=sys.stderr,
        )
        sys.exit(1)
