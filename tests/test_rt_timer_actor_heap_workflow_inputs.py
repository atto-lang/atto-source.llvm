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
    """Reject local source paths crossing directory symlinks."""
    root = root.resolve()
    try:
        parts = path.relative_to(root).parts
    except ValueError as exc:
        raise AssertionError(f"source path outside repository: {path}") from exc
    cursor = root
    for component in parts[:-1]:
        cursor = cursor / component
        if not cursor.resolve().is_relative_to(root):
            raise AssertionError(f"source path traverses repository root: {path}")
        if cursor.is_symlink():
            raise AssertionError(f"symlink path ancestor: {path}")


def local_header_closure(root, paths):
    """Attest local relative and oreslang/-root headers recursively."""
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
            raise AssertionError(f"escaped source path: {item}")
        reject_symlink_ancestors(root, source_file)
        if source_file.is_symlink() or not source_file.is_file():
            raise AssertionError(f"missing or symlink source input: {item}")
        source = source_file.read_text(encoding="utf-8")
        for include in LOCAL_INCLUDE.findall(source):
            if include.startswith("/") or "\\" in include:
                raise AssertionError(f"invalid quoted include: {include}")
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
                    continue  # Known SDK headers, not repository inputs.
                raise AssertionError(f"missing local quoted include: {include}")
            if not candidate.is_file():
                raise AssertionError(f"non-file local quoted include: {include}")
            dependency = resolved.relative_to(root).as_posix()
            if not dependency.startswith(("src/main/cpp/", "tests/")):
                raise AssertionError(f"unattestable local include: {dependency}")
            if dependency not in headers:
                headers.add(dependency)
                pending.append(dependency)
    return headers


def reject_optional_timer_gate(section):
    """Fail closed if timer acceptance has conditional or nonfatal YAML keys."""
    for field in ("if", "continue-on-error"):
        normal = r"(?m)^ {4,}(?:-[ \t]+)?" + re.escape(field) + r"[ \t]*:"
        inline = r"(?m)^ {4,}-[ \t]*\{[^\n}]*\b" + re.escape(field) + r"[ \t]*:"
        assert not (re.search(normal, section) or re.search(inline, section)), (
            f"conditional/nonfatal RT timer gate forbidden: {field}"
        )


def optional_gate_selftest():
    valid = "    runs-on: ubuntu-24.04\n      - name: Execute\n        run: ./unit\n"
    reject_optional_timer_gate(valid)
    reject_optional_timer_gate(valid + '        run: echo "if: is text"\n')
    denied = 0
    for field in ("if", "continue-on-error"):
        for line in (
            f"    {field}: always()\n",
            f"      - {field}: false\n",
            f"        {field}: true\n",
            f"          {field} : always()\n",
            f"      - {{ name: Gate, {field}: false, run: ./unit }}\n",
        ):
            try:
                reject_optional_timer_gate(valid + line)
            except AssertionError as exc:
                assert "forbidden" in str(exc)
                denied += 1
            else:
                raise AssertionError("unsafe optional timer CI gate accepted: " + line)
    assert denied == 10, f"optional gate mutants not all rejected: {denied}"
    print(f"RT_TIMER_OPTIONAL_GATE_SELFTEST_PASS allowed=2 denied={denied}")


# Safety prerequisite: a CMake restack must not drop, bypass, or misbind
# parent actor ledger, fencing, lease authority and scheduler STOP CTests.
ACTOR_CTEST_INPUTS = (
    ("native_actor_execution_ledger_serialization",
     "oreslang-actor-execution-ledger-test",
     "tests/native_actor_execution_ledger_test.cpp"),
    ("native_actor_execution_ledger_deterministic_stress",
     "oreslang-actor-execution-ledger-stress-test",
     "tests/native_actor_execution_ledger_state_stress_test.cpp"),
    ("native_actor_ledger_stop_wake_fencing_interleavings",
     "oreslang-actor-ledger-fencing-interleavings-test",
     "tests/native_actor_ledger_fencing_interleavings_test.cpp"),
    ("native_actor_ledger_lease_authority_boundary",
     "oreslang-actor-ledger-lease-authority-test",
     "tests/native_actor_ledger_lease_authority_test.cpp"),
    ("native_actor_ledger_authenticated_scheduler_bridge",
     "oreslang-actor-ledger-scheduler-bridge-test",
     "tests/native_actor_ledger_scheduler_bridge_test.cpp"),
    ("native_actor_ledger_scheduler_stop_retained_cleanup",
     "oreslang-actor-ledger-scheduler-stop-cleanup-test",
     "tests/native_actor_ledger_scheduler_stop_cleanup_test.cpp"),
)


def require_actor_ctest_registrations(cmake):
    # A commented-out target, fixture or CTest is not an executable gate.
    cmake = re.sub(r"(?m)#[^\n]*$", "", cmake)
    for test_name, target, fixture in ACTOR_CTEST_INPUTS:
        tpat = (r"(?ms)^[ \t]*add_test\([ \t]*NAME[ \t]+"
                + re.escape(test_name)
                + r"(?=\s|\))[ \t\n]+COMMAND[ \t\n]+"
                + re.escape(target) + r"[ \t\n]*\)")
        assert len(re.findall(tpat, cmake)) == 1, (
            f"actor dependency CTest missing or misbound: {test_name}"
        )
        epat = (r"(?ms)^[ \t]*add_executable\([ \t]*"
                + re.escape(target) + r"(?=\s|\))(?P<inputs>[^)]*)\)")
        matches = list(re.finditer(epat, cmake))
        assert len(matches) == 1, (
            f"actor dependency executable missing or duplicated: {target}"
        )
        inputs = matches[0].group("inputs").split()
        assert fixture in inputs, (
            f"actor test {test_name} source target missing {fixture}"
        )


def actor_ctest_selftest():
    good = "".join(
        f"add_executable({target}\n    {fixture})\n"
        f"add_test(NAME {test_name} COMMAND {target})\n"
        for test_name, target, fixture in ACTOR_CTEST_INPUTS
    )
    require_actor_ctest_registrations(good)
    denied = 0
    for test_name, target, fixture in ACTOR_CTEST_INPUTS:
        for mutant in (
            good.replace(f"add_test(NAME {test_name}",
                         f"# add_test(NAME {test_name}"),
            good.replace(fixture, "tests/omitted_stub.cpp"),
            good.replace(f"COMMAND {target})", "COMMAND wrong_target)"),
            good.replace(f"add_executable({target}",
                         f"# add_executable({target}"),
            good + f"\nadd_test(NAME {test_name} COMMAND {target})\n",
        ):
            try:
                require_actor_ctest_registrations(mutant)
            except AssertionError:
                denied += 1
            else:
                raise AssertionError(
                    f"actor CTest source/target bypass: {test_name}"
                )
    assert denied == 30, f"actor CTest mutants escaped: {denied}"
    print("RT_TIMER_PARENT_ACTOR_CTEST_SELFTEST_PASS allowed=1 denied=30")


def audit_selftest():
    actor_ctest_selftest()
    optional_gate_selftest()
    """Source-free negative tests, no private-source export."""
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td).resolve()
        rt = root / "src/main/cpp/oreslang/runtime"
        shared = root / "src/main/cpp/oreslang/shared"
        rt.mkdir(parents=True)
        shared.mkdir(parents=True)
        first = rt / "first.hpp"
        second = rt / "second.hpp"
        third = shared / "third.hpp"
        first.write_text('#include "second.hpp"\n', encoding="utf-8")
        second.write_text('#include "../shared/third.hpp"\n', encoding="utf-8")
        third.write_text("// leaf\n", encoding="utf-8")
        first_path = "src/main/cpp/oreslang/runtime/first.hpp"
        closure = local_header_closure(root, {first_path})
        assert closure == {
            "src/main/cpp/oreslang/runtime/second.hpp",
            "src/main/cpp/oreslang/shared/third.hpp",
        }, f"relative transitive header omitted: {closure}"
        second.write_text('#include "missing.hpp"\n', encoding="utf-8")
        try:
            local_header_closure(root, {first_path})
        except AssertionError as exc:
            assert "missing local" in str(exc)
        else:
            raise AssertionError("missing local quote was accepted")
        second.write_text('#include "../../../../../../escape.hpp"\n',
                          encoding="utf-8")
        try:
            local_header_closure(root, {first_path})
        except AssertionError as exc:
            assert "escapes repository" in str(exc)
        else:
            raise AssertionError("quoted root traversal was accepted")
        second.write_text('#include "alias/third.hpp"\n', encoding="utf-8")
        (rt / "alias").symlink_to(shared, target_is_directory=True)
        try:
            local_header_closure(root, {first_path})
        except AssertionError as exc:
            assert "symlink" in str(exc)
        else:
            raise AssertionError("directory symlink alias was accepted")
    print("RT_TIMER_CI_HEADER_CLOSURE_SELFTEST_PASS "
          "relative_transitive=1 missing=1 traversal=1 symlink=1")


def check():
    audit_selftest()
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
    reject_optional_timer_gate(section)
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
                r"(?m)^\s*(?:(?:ASAN_OPTIONS|UBSAN_OPTIONS)=[^\s]+\s+)*"
                + re.escape(binary_path) + r"\s*$", step
            ), f"{step_name}: {binary} never executed"

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    require_actor_ctest_registrations(cmake)
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
        source_file = root / item
        resolved = source_file.resolve()
        assert resolved.is_relative_to(root), f"escaped source path {item}"
        reject_symlink_ancestors(root, source_file)
        assert source_file.is_file() and not source_file.is_symlink(), (
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
