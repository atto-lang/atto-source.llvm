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
    # A hash-object comparison alone does not reject untracked or modified
    # non-manifest inputs. Require a clean exact-head checkout before builds.
    identity_name = "Verify exact commit and relevant Git blobs"
    identity_marker = f"      - name: {identity_name}\\n"
    assert section.count(identity_marker) == 1, "missing identity gate"
    identity_step = section.split(identity_marker, 1)[1].split(
        "\\n      - name:", 1
    )[0]
    assert re.search(r"(?m)^\\s*set -euo pipefail\\s*$", identity_step), (
        "identity gate is not fail-fast"
    )
    assert re.search(
        r'(?m)^\\s*test -z "\\$\\(git status --porcelain '
        r'--untracked-files=all\\)"\\s*    # manifest. In particular, deleting a timer or heap command must fail the
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
, identity_step
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
