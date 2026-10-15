#!/usr/bin/env python3
"""Fail closed when a CTest configure silently omits required compiler gates.

This checks *registration*, not whether a registered test passed. The caller
must still build all targets and execute the complete CTest suite afterwards.
"""
from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
import subprocess
import sys


class ManifestFailure(RuntimeError):
    """CTest's advertised suite is not a sufficient release-test manifest."""


CRITICAL_TESTS = (
    "native_contract_harness_optimize_failclosed",
    "native_contract_harness_python_O",
    "native_contract_harness_python_OO",
    "native_contract_positive_object_verifies",
    "constant_main",
    "native_rt_while_loop_failclosed",
    "native_actor_owned_heap_contract",
    "native_ctest_manifest_harness_adversarial",
    "native_ownership_cfg_dataflow_fixed_point",
    "portable_target_abi_v1",
    "source_actor_compiled_native_lifecycle",
    "source_actor_compiled_native_asan_ubsan",
    "native_supervisor_hierarchy_control",
    "native_supervisor_priority_control",
    "native_typed_exception_unwind",
    "native_actor_local_unhandled_panic_bypass",
    "native_effect_unwind_supervisor_control_bridge",
    "native_supervisor_failure_lane",
    "native_logical_critical_gate",
    "native_function_effect_bitset_contract",
    "native_critical_state_loan_runtime_bridge",
    "native_actor_owned_typed_scalar_state",
    "native_actor_state_scope_nested_loan_fencing",
    "reject_throw_effect",
    "reject_raise_effect",
    "reject_panic_effect",
)

TIMED_TESTS = frozenset(CRITICAL_TESTS[:4])
# Bind critical CTest *names* to the real commands in CMakeLists.txt. A
# fabricated suite containing 'python -c pass' under a critical name is not
# evidence that the native compiler/actor/supervisor tests were registered.
NATIVE_EXECUTABLES = {
    "native_actor_owned_heap_contract": "oreslang-actor-owned-heap-test",
    "native_ownership_cfg_dataflow_fixed_point": "oreslang-ownership-cfg-dataflow-test",
    "portable_target_abi_v1": "oreslang-target-abi-test",
    "native_supervisor_hierarchy_control": "oreslang-supervisor-hierarchy-test",
    "native_supervisor_priority_control": "oreslang-supervisor-control-turn-test",
    "native_typed_exception_unwind": "oreslang-effect-unwind-test",
    "native_actor_local_unhandled_panic_bypass": "oreslang-actor-local-failure-test",
    "native_effect_unwind_supervisor_control_bridge": "oreslang-effect-supervision-bridge-test",
    "native_supervisor_failure_lane": "oreslang-supervisor-failure-lane-test",
    "native_logical_critical_gate": "oreslang-critical-gate-test",
    "native_function_effect_bitset_contract": "oreslang-function-effects-test",
    "native_critical_state_loan_runtime_bridge": "oreslang-critical-state-loan-bridge-test",
    "native_actor_owned_typed_scalar_state": "oreslang-owned-scalar-state-test",
    "native_actor_state_scope_nested_loan_fencing": "oreslang-actor-state-scope-test",
}
GUEST_FIXTURES = {
    "constant_main": ("constant-main.ores", "--ir"),
    "native_rt_while_loop_failclosed": ("native-rt-while-reject.ores", "--diagnostic"),
    "reject_throw_effect": ("unsupported-throw-effect.ores", "--diagnostic"),
    "reject_raise_effect": ("unsupported-raise-effect.ores", "--diagnostic"),
    "reject_panic_effect": ("unsupported-panic-effect.ores", "--diagnostic"),
}
SOURCE_ACTOR_TESTS = frozenset({
    "source_actor_compiled_native_lifecycle",
    "source_actor_compiled_native_asan_ubsan",
})


def python_program(program: str) -> bool:
    # CTest exposes an absolute interpreter path on the Ubuntu build runner.
    return re.fullmatch(r"python(?:3(?:\.\d+)?)?", Path(program).name) is not None


def option_value(command: list[str], flag: str, test: str) -> str:
    if command.count(flag) != 1:
        raise ManifestFailure(f"critical CTest must have exactly one {flag}: {test}")
    index = command.index(flag)
    if index + 1 == len(command) or not command[index + 1] or command[index + 1].startswith("-"):
        raise ManifestFailure(f"critical CTest has no value for {flag}: {test}")
    return command[index + 1]


def require_real_critical_command(name: str, command: list[str]) -> None:
    """Check CTest registration targets the intended executable/fixture.

    This is deliberately a manifest check, not binary provenance or evidence
    that the command executed. Full CTest and independent CI remain mandatory.
    """
    if name in NATIVE_EXECUTABLES:
        if (Path(command[0]).name != NATIVE_EXECUTABLES[name]
                or (len(command) != 2 if name == "portable_target_abi_v1"
                    else len(command) != 1)):
            raise ManifestFailure(f"critical native test targets wrong executable: {name}")
        if name == "portable_target_abi_v1" and not command[1].endswith(
                "/contracts/architecture/v1/targets.tsv"):
            raise ManifestFailure("target ABI test targets wrong contract file")
    elif name in GUEST_FIXTURES:
        fixture, evidence = GUEST_FIXTURES[name]
        if (len(command) < 7 or not python_program(command[0])
                or not command[1].endswith("/tests/contract.py")
                or Path(option_value(command, "--compiler", name)).name != "oreslang-llvmc"
                or not option_value(command, "--fixture", name).endswith(
                    "/tests/fixtures/" + fixture)):
            raise ManifestFailure(f"critical guest contract targets wrong compiler/fixture: {name}")
        option_value(command, evidence, name)
        if evidence == "--diagnostic" and "--verify-object" in command:
            raise ManifestFailure(f"negative guest fixture also requests a positive object: {name}")
    elif name in SOURCE_ACTOR_TESTS:
        if (len(command) < 8 or not python_program(command[0])
                or not command[1].endswith("/tests/source_actor_e2e.py")
                or Path(option_value(command, "--compiler", name)).name != "oreslang-llvmc"
                or not option_value(command, "--clang", name)
                or not option_value(command, "--root", name)):
            raise ManifestFailure(f"compiled actor test does not invoke source harness: {name}")
        if (("--sanitize" in command)
                != (name == "source_actor_compiled_native_asan_ubsan")):
            raise ManifestFailure(f"source actor sanitizer mode differs from test name: {name}")
    elif name.startswith("native_contract_harness_"):
        optimization = {
            "native_contract_harness_optimize_failclosed": [],
            "native_contract_harness_python_O": ["-O"],
            "native_contract_harness_python_OO": ["-OO"],
        }[name]
        if (not python_program(command[0])
                or command[1:] != optimization + [command[-1]]
                or not command[-1].endswith("/tests/test_contract_harness.py")):
            raise ManifestFailure(f"optimized harness has wrong interpreter/arguments: {name}")
    elif name == "native_ctest_manifest_harness_adversarial":
        if (not python_program(command[0])
                or command[1:4] != ["-m", "unittest", "discover"]
                or option_value(command, "-p", name) != "test_ctest_manifest_gate.py"
                or not option_value(command, "-s", name).endswith("/tests")
                or "-v" not in command):
            raise ManifestFailure("manifest adversarial test does not run its test suite")
    elif name == "native_contract_positive_object_verifies":
        if (not python_program(command[0])
                or not command[1].endswith("/tests/contract.py")
                or Path(option_value(command, "--compiler", name)).name != "oreslang-llvmc"
                or not option_value(command, "--fixture", name).endswith(
                    "/tests/fixtures/constant-main.ores")
                or not option_value(command, "--clang", name)
                or "--verify-object" not in command
                or "--diagnostic" in command):
            raise ManifestFailure("positive source object gate does not compile its guest fixture")
    else:
        raise ManifestFailure(f"unclassified critical CTest: {name}")


UNSAFE_PROPERTIES = frozenset((
    "DISABLED", "WILL_FAIL", "SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION",
))


def check_manifest(manifest: object, *, min_tests: int = 250,
                   required: tuple[str, ...] = CRITICAL_TESTS) -> int:
    if not isinstance(manifest, dict) or not isinstance(manifest.get("tests"), list):
        raise ManifestFailure("CTest JSON must have a tests array")
    version = manifest.get("version")
    if (manifest.get("kind") != "ctestInfo" or not isinstance(version, dict)
            or type(version.get("major")) is not int or version["major"] != 1
            or type(version.get("minor")) is not int or version["minor"] < 0):
        raise ManifestFailure("CTest JSON version/kind is not the supported schema")
    tests = manifest["tests"]
    if len(tests) < min_tests:
        raise ManifestFailure(
            f"CTest registered {len(tests)} tests; expected at least {min_tests}"
        )
    by_name: dict[str, dict] = {}
    for entry in tests:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            raise ManifestFailure("CTest test without a valid name")
        name = entry["name"]
        if not name or name in by_name:
            raise ManifestFailure(f"empty or duplicate CTest name: {name!r}")
        by_name[name] = entry
    # A nominally green CTest run must not hide unresolved executables or
    # disabled/skipped tests outside the mandatory contract set.
    for name, entry in by_name.items():
        command = entry.get("command")
        if (not isinstance(command, list) or not command
                or not all(isinstance(x, str) and x for x in command)
                or any("NOTFOUND" in x or "$<" in x for x in command)):
            raise ManifestFailure(f"CTest has unresolved/empty command: {name}")
        properties = entry.get("properties", [])
        if not isinstance(properties, list):
            raise ManifestFailure(f"CTest has invalid properties: {name}")
        for prop in properties:
            if not isinstance(prop, dict) or not isinstance(prop.get("name"), str):
                raise ManifestFailure(f"CTest has malformed property: {name}")
            key = prop["name"]
            value = prop.get("value")
            if key in ("SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION"):
                raise ManifestFailure(f"CTest has skip policy {key}: {name}")
            if key in ("DISABLED", "WILL_FAIL") and str(value).lower() in (
                    "true", "on", "yes", "1"):
                raise ManifestFailure(f"CTest is disabled/inverted: {name}, {key}")

    for name in required:
        if name not in by_name:
            raise ManifestFailure(f"required CTest was not registered: {name}")
        entry = by_name[name]
        command = entry.get("command")
        if (not isinstance(command, list) or not command
                or not all(isinstance(x, str) and x for x in command)
                or any("NOTFOUND" in x or "$<" in x for x in command)):
            raise ManifestFailure(f"CTest has unresolved/empty command: {name}")
        # A test of the right *name* can still silently do the wrong thing.
        # Preserve Python -O/-OO and the real native-object positive control.
        if name.startswith("native_contract_harness_"):
            if not any(arg.endswith("test_contract_harness.py") for arg in command):
                raise ManifestFailure(f"optimized harness command targets wrong script: {name}")
        for mode_name, flag in (
                ("native_contract_harness_python_O", "-O"),
                ("native_contract_harness_python_OO", "-OO")):
            if name == mode_name:
                script_index = next((i for i, arg in enumerate(command)
                                     if arg.endswith("test_contract_harness.py")), -1)
                if script_index <= 0 or flag not in command[1:script_index]:
                    raise ManifestFailure(
                        f"optimized harness is not invoked with Python {flag}")
        if name == "native_contract_positive_object_verifies":
            for flag in ("--verify-object", "--clang", "--compiler", "--fixture"):
                if flag not in command:
                    raise ManifestFailure(f"source object CTest is missing {flag}")
                if flag != "--verify-object":
                    index = command.index(flag)
                    if index + 1 >= len(command) or command[index + 1].startswith("--"):
                        raise ManifestFailure(f"source object CTest has no value for {flag}")
            if not any(arg.endswith("contract.py") for arg in command):
                raise ManifestFailure("source object CTest does not invoke contract.py")
            if not any(arg.endswith("constant-main.ores") for arg in command):
                raise ManifestFailure("source object CTest does not compile positive guest source")
        if name == "source_actor_compiled_native_asan_ubsan" and "--sanitize" not in command:
            raise ManifestFailure("source actor sanitizer CTest is missing --sanitize")
        properties = entry.get("properties", [])
        if not isinstance(properties, list):
            raise ManifestFailure(f"CTest has invalid properties: {name}")
        props: dict[str, object] = {}
        for prop in properties:
            if (not isinstance(prop, dict)
                    or not isinstance(prop.get("name"), str)
                    or prop["name"] in props):
                raise ManifestFailure(f"CTest has malformed/duplicate property: {name}")
            props[prop["name"]] = prop.get("value")
        # Do not let CTest count a disabled, skipped, or expected-to-fail
        # critical test as executable positive proof.
        for key in UNSAFE_PROPERTIES:
            if key in props and key not in ("DISABLED", "WILL_FAIL"):
                raise ManifestFailure(f"critical CTest has skip policy {key}: {name}")
            if key in ("DISABLED", "WILL_FAIL") and str(props.get(key, "")).lower() in (
                    "true", "on", "yes", "1"):
                raise ManifestFailure(f"critical CTest is disabled/inverted: {name}, {key}")
        if name in TIMED_TESTS:
            value = props.get("TIMEOUT")
            try:
                timeout = float(value)
            except (ValueError, TypeError):
                raise ManifestFailure(f"critical CTest lacks numeric timeout: {name}")
            if not math.isfinite(timeout) or timeout <= 0 or timeout > 300:
                raise ManifestFailure(f"critical CTest timeout is unsafe: {name}")
        # Validate actual CMake test executables and fixtures, not names alone.
        require_real_critical_command(name, command)
    return len(tests)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--min-tests", type=int, default=250)
    args = parser.parse_args()
    if args.min_tests < len(CRITICAL_TESTS):
        raise ManifestFailure("minimum test count is below the critical corpus")
    build = args.build_dir.resolve(strict=True)
    if not build.is_dir():
        raise ManifestFailure("CTest build directory is missing")
    probe = subprocess.run(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
        capture_output=True, text=True, check=False, timeout=60,
    )
    if probe.returncode:
        raise ManifestFailure("CTest enumeration failed: " + probe.stderr)
    try:
        document = json.loads(probe.stdout)
    except json.JSONDecodeError as exc:
        raise ManifestFailure("CTest did not provide valid JSON") from exc
    count = check_manifest(document, min_tests=args.min_tests)
    print(f"CTEST_MANIFEST_GATE=PASS tests={count} critical={len(CRITICAL_TESTS)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ManifestFailure, OSError, subprocess.TimeoutExpired) as exc:
        print(f"CTEST_MANIFEST_GATE=FAIL {exc}", file=sys.stderr)
        sys.exit(1)
