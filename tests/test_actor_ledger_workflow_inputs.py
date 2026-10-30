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
LOCAL_INCLUDE = re.compile(r'(?m)^[ \t]*#[ \t]*include[ \t]+"([^"\r\n]+)"')
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


def verify_executed_blob_comparison(section, job):
    """Require an executable, fail-fast Git blob equality guard per input.

    Listing paths in a shell loop is insufficient if a later workflow edit
    removes the comparison or converts it into an echo-only attestation.
    """
    marker = "for path in " + "\\" + "\n"
    assert section.count(marker) == 1, f"{job}: missing/ambiguous blob loop"
    prefix, tail = section.split(marker, 1)
    assert "set -euo pipefail" in prefix, f"{job}: non-fail-fast verification"
    assert 'git status --porcelain --untracked-files=all' in prefix, (
        f"{job}: missing clean-checkout verification"
    )
    assert "persist-credentials: false" in prefix, (
        f"{job}: checkout retained credentials"
    )
    assert "github.event.pull_request.head.sha || github.sha" in prefix, (
        f"{job}: checkout not bound to immutable PR head"
    )
    assert "; do\n" in tail, f"{job}: blob loop has no body"
    loop_body = tail.split("; do\n", 1)[1]
    end = re.search(r"(?m)^[ \t]*done[ \t]*$", loop_body)
    assert end, f"{job}: unterminated blob loop"
    lines = {
        line.strip() for line in loop_body[:end.start()].splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    direct = (
        'test "$(git hash-object "$path")" = "$(git rev-parse "HEAD:$path")"'
    )
    staged = {
        'expected="$(git rev-parse "HEAD:$path")"',
        'actual="$(git hash-object "$path")"',
        'test "$actual" = "$expected"',
    }
    assert direct in lines or staged.issubset(lines), (
        f"{job}: immutable blob equality check removed or disabled"
    )


def require_guest_gate_dependency(section):
    """Guest compilation must never outrun the native integrity/safety gate."""
    needs = re.findall(
        r"(?m)^    needs:[ \t]*([a-z][a-z0-9-]*)[ \t]*$", section
    )
    assert needs == ["actor-ledger-contract"], (
        "compiled guest job must depend on exactly actor-ledger-contract"
    )


def reject_conditional_gate_bypass(sections):
    """Safety and identity acceptance steps must be unconditional and fatal.

    Job-level if: always() can override a failed needs prerequisite, while
    step-level conditions and continue-on-error can skip critical checks.
    """
    for job in REQUIRED_JOBS:
        section = sections[job]
        for field in ("if", "continue-on-error"):
            pattern = rf"(?m)^ {{4,}}(?:-[ ]+)?{field} *:"
            assert re.search(pattern, section) is None, (
                f"{job}: conditional/non-fatal CI gate forbidden: {field}"
            )

REQUIRED_EXECUTED_COMMANDS = {
    "actor-ledger-contract": (
        "python3 -B tests/test_actor_ledger_workflow_inputs.py",
        '"build/actor-ledger-gate/test-${cxx//+/p}"',
        '"build/actor-ledger-gate/stress-${cxx//+/p}"',
        '"build/actor-ledger-gate/fence-${cxx//+/p}"',
        "build/actor-ledger-gate/test-sanitized",
        "build/actor-ledger-gate/stress-sanitized",
        "build/actor-ledger-gate/fence-sanitized",
    ),
    "compiled-ores-guest": (
        "cmake --build build/actor-ledger-guest --target oreslang-llvmc -j 2",
        "python3 tests/native_actor_compiled_guest_ledger.py",
        '--clang clang --root "$PWD" --sanitize',
    ),
}

def verify_executable_acceptance_steps(sections):
    """Reject deletion or echo-only replacement of required test invocations.

    Required lines must occur in a workflow shell run body (not a comment,
    metadata path list, or an echo containing a test command). This is an
    admission guard, not proof that hosted runners actually executed them.
    """
    for job, required in REQUIRED_EXECUTED_COMMANDS.items():
        section = sections[job]
        shell_lines = [
            line.strip().rstrip("\\").rstrip()
            for line in section.splitlines()
            if re.match(r"^ {10,}", line) and not line.lstrip().startswith("#")
        ]
        for command in required:
            count = shell_lines.count(command)
            minimum = (2 if (job == "compiled-ores-guest" and
                            command == "python3 tests/native_actor_compiled_guest_ledger.py")
                       else 1)
            assert count >= minimum, (
                f"{job}: required executable test command missing: {command}"
            )

def audit_selftest():
    # If test commands are removed, commented or replaced with echo, a
    # passing blob manifest must not produce a green acceptance job.
    commands_fixture = {
        job: '\n'.join('          ' + item for item in commands) + '\n'
        for job, commands in REQUIRED_EXECUTED_COMMANDS.items()
    }
    commands_fixture["compiled-ores-guest"] += (
        "          python3 tests/native_actor_compiled_guest_ledger.py\n"
    )
    verify_executable_acceptance_steps(commands_fixture)
    for job, commands in REQUIRED_EXECUTED_COMMANDS.items():
        for cmd in commands:
            damaged = dict(commands_fixture)
            damaged[job] = damaged[job].replace("          " + cmd,
                "          echo " + cmd, 2)
            try:
                verify_executable_acceptance_steps(damaged)
            except AssertionError as exc:
                assert "required executable test command missing" in str(exc)
            else:
                raise AssertionError(f"echo-only acceptance command accepted: {job} {cmd}")
    valid = {
        "actor-ledger-contract": "    runs-on: ubuntu-24.04\n      - name: Gate\n        run: echo gate\n",
        "compiled-ores-guest": "    needs: actor-ledger-contract\n      - name: Guest\n        run: echo guest\n",
    }
    reject_conditional_gate_bypass(valid)
    for job in REQUIRED_JOBS:
        for field in ("if", "continue-on-error"):
            for declaration in (
                f"    {field}: always()\n",
                f"      - {field}: false\n",
                f"        {field}: true\n",
                f"          {field}: always()\n",
            ):
                unsafe = dict(valid)
                unsafe[job] += declaration
                try:
                    reject_conditional_gate_bypass(unsafe)
                except AssertionError as exc:
                    assert "conditional/non-fatal CI gate forbidden" in str(exc)
                else:
                    raise AssertionError(f"bypassed CI gate accepted: {job} {declaration}")
    require_guest_gate_dependency("    needs: actor-ledger-contract\n")
    for unsafe in ("    runs-on: ubuntu-24.04\n",
                   "    needs: unrelated-job\n",
                   "    # needs: actor-ledger-contract\n"):
        try:
            require_guest_gate_dependency(unsafe)
        except AssertionError as exc:
            assert "must depend" in str(exc)
        else:
            raise AssertionError("unguarded guest job dependency accepted")
    directory = "src/main/cpp/oreslang/runtime"
    assert triggered(f"{directory}/added/new.hpp", [f"{directory}/**"])
    assert not triggered(f"{directory}-evil/new.hpp", [f"{directory}/**"])
    assert not triggered(f"{directory}2/new.hpp", [f"{directory}/**"])
    assert triggered("tests/some_test.cpp", ["tests/**"])
    assert not triggered("tests-fake/some_test.cpp", ["tests/**"])
    assert not triggered("tests/new/helper.hpp", ["tests/test_actor_ledger_workflow_inputs.py"])
    assert triggered("src/main/cpp/main.cpp", ["src/main/cpp/main.cpp"])
    # Catch a workflow edit that lists inputs but removes hash comparison.
    prefix = (
        'persist-credentials: false\n'
        'ref: github.event.pull_request.head.sha || github.sha\n'
        'set -euo pipefail\n'
        'test -z "$(git status --porcelain --untracked-files=all)"\n'
        'for path in ' + '\\' + '\n'
        'tests/fixture.c; do\n'
    )
    direct = (
        '  test "$(git hash-object "$path")" = '
        '"$(git rev-parse "HEAD:$path")"\n'
    )
    verify_executed_blob_comparison(
        prefix + direct + 'done\n', 'selftest-direct'
    )
    staged = (
        '  expected="$(git rev-parse "HEAD:$path")"\n'
        '  actual="$(git hash-object "$path")"\n'
        '  test "$actual" = "$expected"\n'
    )
    verify_executed_blob_comparison(
        prefix + staged + 'done\n', 'selftest-staged'
    )
    try:
        verify_executed_blob_comparison(
            prefix.replace("set -euo pipefail", "set +e")
            + direct + 'done\n',
            'selftest-nonfailfast',
        )
    except AssertionError as exc:
        assert "non-fail-fast" in str(exc)
    else:
        raise AssertionError("non-fail-fast blob verification not rejected")
    try:
        verify_executed_blob_comparison(
            prefix + '  echo "EXACT_BLOB $path"\n' + 'done\n',
            'selftest-no-comparison',
        )
    except AssertionError as exc:
        assert "equality check" in str(exc)
    else:
        raise AssertionError("echo-only blob attestation not rejected")
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
        "ACTOR_CI_HEADER_CLOSURE_SELFTEST_PASS "
        "wildcard=6 relative_cycle=1 transitive_omission=1 "
        "traversal=1 missing_owned=1 symlink_ancestors=2"
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
    assert "tests/**" in trigger_set, "missing broad tests/** CI trigger"

    boundaries = list(JOB.finditer(body))
    sections = {
        match.group(1): body[match.end(): (
            boundaries[i + 1].start()
            if i + 1 < len(boundaries) else len(body)
        )]
        for i, match in enumerate(boundaries)
    }
    assert set(REQUIRED_JOBS) <= sections.keys(), "missing mandatory jobs"
    require_guest_gate_dependency(sections["compiled-ores-guest"])
    reject_conditional_gate_bypass(sections)
    verify_executable_acceptance_steps(sections)
    all_inputs = set()
    for name in REQUIRED_JOBS:
        section = sections[name]
        verify_executed_blob_comparison(section, name)
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
