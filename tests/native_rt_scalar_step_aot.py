#!/usr/bin/env python3
"""Compile actual opt-in Oreslang source and drive two native heap continuations."""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, check=False,
                          timeout=60, **kwargs)

def parse_clang_major(output):
    """Parse clang -dumpversion without silently accepting an unknown SDK."""
    match = re.fullmatch(r'\s*([0-9]+)(?:\.[0-9]+)*\s*', output)
    if not match:
        raise ValueError("unrecognized clang -dumpversion output")
    return int(match.group(1))


def verify_matching_llvm_clang(clang, expected_major):
    version = run([clang, "-dumpversion"])
    if version.returncode != 0 or version.stderr.strip():
        raise RuntimeError("cannot certify the selected Clang version")
    actual = parse_clang_major(version.stdout)
    if actual != expected_major:
        raise RuntimeError(
            f"LLVM SDK major {expected_major} mismatches Clang major {actual}; "
            "not valid native AOT certification")
    return actual


def verify_closed_guest_ir(source):
    """Reject any guest ABI/import/call surface beyond the scalar v1 subset.

    This is a readable regression oracle in addition to the actual LLVM
    module verifier. It does not replace optimized executable guest tests.
    """
    definitions = set(re.findall(r'(?m)^define\s+[^@\n]+@([A-Za-z0-9_.]+)\(', source))
    expected = {
        "oreslang_rt_scalar_frame_init_v1",
        "oreslang_rt_scalar_frame_step_v1",
        "oreslang_rt_scalar_frame_cancel_v1",
        "oreslang_rt_scalar_frame_result_v1",
    }
    if definitions != expected:
        raise AssertionError(("unapproved compiled guest function", definitions))
    # A name-only whitelist can miss a same-named function with a wrong
    # signature. Check the actual v1 entrypoint ABI for the C11 carrier.
    signatures = re.findall(
        r'(?m)^define\s+(void|i32|i64)\s+@([A-Za-z0-9_.]+)\(([^)]*)\)', source)
    if len(signatures) != 4:
        raise AssertionError(("invalid scalar guest function signatures", signatures))
    expected_abi = {
        "oreslang_rt_scalar_frame_init_v1": ("void", 2),
        "oreslang_rt_scalar_frame_step_v1": ("i32", 2),
        "oreslang_rt_scalar_frame_cancel_v1": ("void", 1),
        "oreslang_rt_scalar_frame_result_v1": ("i64", 1),
    }
    # LLVM 14 typed pointers use %frame*, newer opaque-pointer LLVM uses ptr.
    # An i64* parameter is not an acceptable frame pointer for struct GEP.
    frame_pointer = re.compile(
        r'^(?:ptr|%"?oreslang\.rt\.scalar\.frame\.v1"?\*)'
        r'(?:\s+%[A-Za-z0-9_.]+)?    if declarations != {"llvm.trap"}:
        raise AssertionError(("unapproved guest import or FFI", declarations))
    if re.search(r'(?m)^\s*(?:%[\w.]+\s*=\s*)?(?:invoke|callbr)\b', source):
        raise AssertionError("unverified interprocedural transfer")
    calls = re.findall(
        r'(?m)^\s*(?:%[\w.]+\s*=\s*)?(?:(?:tail|musttail|notail)\s+)?call\b[^\n]*',
        source,
    )
    for call in calls:
        if not re.search(r'@llvm\.trap\(', call):
            raise AssertionError(("unapproved guest call", call))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--llvm-major", required=True, type=int)
    p.add_argument("--fixture", required=True)
    p.add_argument("--harness", required=True)
    args = p.parse_args()
    # A generic clang may be older/newer than the C++ LLVM SDK. Do not
    # certify generated IR against a mismatched native toolchain.
    verify_matching_llvm_clang(args.clang, args.llvm_major)
    compiler = args.compiler
    fixture = pathlib.Path(args.fixture)
    cases = [
        ("val count: int = 0;", "val count: int = -1;"),
        ("count = count + 1;", "count = count + 2;"),
        ("while (count < 1000)", "while (count < 2000000000)"),
        ("return count;", "return 17;"),
        ("val count: int = 0;", "const count: int = 0;"),
        ("count = count + 1;", "other = other + 1;"),
        ("while (count < 1000)", "while (count < 1000) { count = count + 1; } while (count < 2000)"),
    ]
    source = fixture.read_text()
    with tempfile.TemporaryDirectory(prefix="oreslang-rt-frame-") as folder:
        out = pathlib.Path(folder)
        def compile_source(path, gated=True):
            return run([compiler] + (["--experimental-rt-scalar-loop"] if gated else []) + [str(path)])
        denied = compile_source(fixture, False)
        assert denied.returncode != 0 and not denied.stdout.strip(), "default loop gate reopened"
        assert "requires verified LLVM continuation lowering" in denied.stderr, denied.stderr
        accepted = compile_source(fixture)
        assert accepted.returncode == 0, accepted.stderr
        assert "define i32 @oreslang_rt_scalar_frame_step_v1" in accepted.stdout, "not compiled guest step"
        assert "define void @oreslang_rt_scalar_frame_cancel_v1" in accepted.stdout
        assert "define void @oreslang_rt_scalar_frame_init_v1" in accepted.stdout
        assert "define i64 @oreslang_rt_scalar_frame_result_v1" in accepted.stdout
        # Reject the v0 24-byte ABI: an old caller must fail to link rather
        # than corrupt the extended 32-byte persistent-fuel guest frame.
        assert "define void @oreslang_rt_scalar_frame_init(" not in accepted.stdout
        assert "define i32 @oreslang_rt_scalar_frame_step(" not in accepted.stdout
        assert "fuel.remaining.logical" in accepted.stdout, "missing committed lifetime fuel debit"
        assert "fuel.logical.exhausted" in accepted.stdout, "missing immutable quota denial"
        verify_closed_guest_ir(accepted.stdout)
        llvm_ir = out / "guest.ll"
        llvm_ir.write_text(accepted.stdout)
        for optimization in ("-O0", "-O2", "-O3"):
            exe = out / f"run-guest-{optimization[2:]}"
            build = run([args.clang, "-std=c11", optimization, "-DNDEBUG",
                         "-Wall", "-Wextra", "-Werror",
                         "-Wno-error=override-module", "-pedantic",
                         str(args.harness), "-x", "ir", str(llvm_ir),
                         "-o", str(exe)])
            assert build.returncode == 0, (optimization, build.stderr)
            execution = run([str(exe)])
            assert execution.returncode == 0, (optimization, execution.stderr)
            assert "SCALAR_FRAME_AOT_TWO_TASKS_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_THREE_PEERS_BOUNDED_PROGRESS_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_CANCELLED_PEER_ISOLATION_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_LOGICAL_FUEL_CONSERVATION_PASS" in execution.stdout
        for i, (original, replacement) in enumerate(cases):
            assert original in source
            file = out / f"invalid-{i}.ores"
            file.write_text(source.replace(original, replacement, 1))
            wrong = compile_source(file)
            assert wrong.returncode != 0 and not wrong.stdout.strip(), (i, wrong.stdout)
        missing = run([compiler, "--experimental-rt-scalar-loop", "--target=wasm32", str(fixture)])
        assert missing.returncode != 0 and not missing.stdout.strip()
        print("NATIVE_OPT_IN_SCALAR_FRAME_PASS: O0/O2/O3, 3 peers, conserved logical fuel, cancellation, negative traps")

if __name__ == "__main__":
    # This legacy harness still has many assertion-based end-to-end checks.
    # Never report a false PASS when PYTHONOPTIMIZE/-O would disable them.
    if sys.flags.optimize:
        raise SystemExit("native AOT security tests require Python assertions enabled")
    main()
)
    i64_argument = re.compile(r'^i64(?:\s+%[A-Za-z0-9_.]+)?    if declarations != {"llvm.trap"}:
        raise AssertionError(("unapproved guest import or FFI", declarations))
    if re.search(r'(?m)^\s*(?:%[\w.]+\s*=\s*)?(?:invoke|callbr)\b', source):
        raise AssertionError("unverified interprocedural transfer")
    calls = re.findall(
        r'(?m)^\s*(?:%[\w.]+\s*=\s*)?(?:(?:tail|musttail|notail)\s+)?call\b[^\n]*',
        source,
    )
    for call in calls:
        if not re.search(r'@llvm\.trap\(', call):
            raise AssertionError(("unapproved guest call", call))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--llvm-major", required=True, type=int)
    p.add_argument("--fixture", required=True)
    p.add_argument("--harness", required=True)
    args = p.parse_args()
    # A generic clang may be older/newer than the C++ LLVM SDK. Do not
    # certify generated IR against a mismatched native toolchain.
    verify_matching_llvm_clang(args.clang, args.llvm_major)
    compiler = args.compiler
    fixture = pathlib.Path(args.fixture)
    cases = [
        ("val count: int = 0;", "val count: int = -1;"),
        ("count = count + 1;", "count = count + 2;"),
        ("while (count < 1000)", "while (count < 2000000000)"),
        ("return count;", "return 17;"),
        ("val count: int = 0;", "const count: int = 0;"),
        ("count = count + 1;", "other = other + 1;"),
        ("while (count < 1000)", "while (count < 1000) { count = count + 1; } while (count < 2000)"),
    ]
    source = fixture.read_text()
    with tempfile.TemporaryDirectory(prefix="oreslang-rt-frame-") as folder:
        out = pathlib.Path(folder)
        def compile_source(path, gated=True):
            return run([compiler] + (["--experimental-rt-scalar-loop"] if gated else []) + [str(path)])
        denied = compile_source(fixture, False)
        assert denied.returncode != 0 and not denied.stdout.strip(), "default loop gate reopened"
        assert "requires verified LLVM continuation lowering" in denied.stderr, denied.stderr
        accepted = compile_source(fixture)
        assert accepted.returncode == 0, accepted.stderr
        assert "define i32 @oreslang_rt_scalar_frame_step_v1" in accepted.stdout, "not compiled guest step"
        assert "define void @oreslang_rt_scalar_frame_cancel_v1" in accepted.stdout
        assert "define void @oreslang_rt_scalar_frame_init_v1" in accepted.stdout
        assert "define i64 @oreslang_rt_scalar_frame_result_v1" in accepted.stdout
        # Reject the v0 24-byte ABI: an old caller must fail to link rather
        # than corrupt the extended 32-byte persistent-fuel guest frame.
        assert "define void @oreslang_rt_scalar_frame_init(" not in accepted.stdout
        assert "define i32 @oreslang_rt_scalar_frame_step(" not in accepted.stdout
        assert "fuel.remaining.logical" in accepted.stdout, "missing committed lifetime fuel debit"
        assert "fuel.logical.exhausted" in accepted.stdout, "missing immutable quota denial"
        verify_closed_guest_ir(accepted.stdout)
        llvm_ir = out / "guest.ll"
        llvm_ir.write_text(accepted.stdout)
        for optimization in ("-O0", "-O2", "-O3"):
            exe = out / f"run-guest-{optimization[2:]}"
            build = run([args.clang, "-std=c11", optimization, "-DNDEBUG",
                         "-Wall", "-Wextra", "-Werror",
                         "-Wno-error=override-module", "-pedantic",
                         str(args.harness), "-x", "ir", str(llvm_ir),
                         "-o", str(exe)])
            assert build.returncode == 0, (optimization, build.stderr)
            execution = run([str(exe)])
            assert execution.returncode == 0, (optimization, execution.stderr)
            assert "SCALAR_FRAME_AOT_TWO_TASKS_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_THREE_PEERS_BOUNDED_PROGRESS_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_CANCELLED_PEER_ISOLATION_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_LOGICAL_FUEL_CONSERVATION_PASS" in execution.stdout
        for i, (original, replacement) in enumerate(cases):
            assert original in source
            file = out / f"invalid-{i}.ores"
            file.write_text(source.replace(original, replacement, 1))
            wrong = compile_source(file)
            assert wrong.returncode != 0 and not wrong.stdout.strip(), (i, wrong.stdout)
        missing = run([compiler, "--experimental-rt-scalar-loop", "--target=wasm32", str(fixture)])
        assert missing.returncode != 0 and not missing.stdout.strip()
        print("NATIVE_OPT_IN_SCALAR_FRAME_PASS: O0/O2/O3, 3 peers, conserved logical fuel, cancellation, negative traps")

if __name__ == "__main__":
    # This legacy harness still has many assertion-based end-to-end checks.
    # Never report a false PASS when PYTHONOPTIMIZE/-O would disable them.
    if sys.flags.optimize:
        raise SystemExit("native AOT security tests require Python assertions enabled")
    main()
)
    visited = set()
    for result_type, name, arguments in signatures:
        if name in visited or name not in expected_abi:
            raise AssertionError(("duplicate/unapproved scalar guest ABI", name))
        visited.add(name)
        expected_return, expected_arity = expected_abi[name]
        parameters = [part.strip() for part in arguments.split(',')]
        if (result_type != expected_return or
                len(parameters) != expected_arity or
                not frame_pointer.fullmatch(parameters[0]) or
                (expected_arity == 2 and
                 not i64_argument.fullmatch(parameters[1]))):
            raise AssertionError(("invalid scalar guest frame ABI", name, arguments))

    declarations = set(re.findall(r'(?m)^declare\s+[^@\n]+@([A-Za-z0-9_.]+)\(', source))
    if declarations != {"llvm.trap"}:
        raise AssertionError(("unapproved guest import or FFI", declarations))
    if re.search(r'(?m)^\s*(?:%[\w.]+\s*=\s*)?(?:invoke|callbr)\b', source):
        raise AssertionError("unverified interprocedural transfer")
    calls = re.findall(
        r'(?m)^\s*(?:%[\w.]+\s*=\s*)?(?:(?:tail|musttail|notail)\s+)?call\b[^\n]*',
        source,
    )
    for call in calls:
        if not re.search(r'@llvm\.trap\(', call):
            raise AssertionError(("unapproved guest call", call))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--llvm-major", required=True, type=int)
    p.add_argument("--fixture", required=True)
    p.add_argument("--harness", required=True)
    args = p.parse_args()
    # A generic clang may be older/newer than the C++ LLVM SDK. Do not
    # certify generated IR against a mismatched native toolchain.
    verify_matching_llvm_clang(args.clang, args.llvm_major)
    compiler = args.compiler
    fixture = pathlib.Path(args.fixture)
    cases = [
        ("val count: int = 0;", "val count: int = -1;"),
        ("count = count + 1;", "count = count + 2;"),
        ("while (count < 1000)", "while (count < 2000000000)"),
        ("return count;", "return 17;"),
        ("val count: int = 0;", "const count: int = 0;"),
        ("count = count + 1;", "other = other + 1;"),
        ("while (count < 1000)", "while (count < 1000) { count = count + 1; } while (count < 2000)"),
    ]
    source = fixture.read_text()
    with tempfile.TemporaryDirectory(prefix="oreslang-rt-frame-") as folder:
        out = pathlib.Path(folder)
        def compile_source(path, gated=True):
            return run([compiler] + (["--experimental-rt-scalar-loop"] if gated else []) + [str(path)])
        denied = compile_source(fixture, False)
        assert denied.returncode != 0 and not denied.stdout.strip(), "default loop gate reopened"
        assert "requires verified LLVM continuation lowering" in denied.stderr, denied.stderr
        accepted = compile_source(fixture)
        assert accepted.returncode == 0, accepted.stderr
        assert "define i32 @oreslang_rt_scalar_frame_step_v1" in accepted.stdout, "not compiled guest step"
        assert "define void @oreslang_rt_scalar_frame_cancel_v1" in accepted.stdout
        assert "define void @oreslang_rt_scalar_frame_init_v1" in accepted.stdout
        assert "define i64 @oreslang_rt_scalar_frame_result_v1" in accepted.stdout
        # Reject the v0 24-byte ABI: an old caller must fail to link rather
        # than corrupt the extended 32-byte persistent-fuel guest frame.
        assert "define void @oreslang_rt_scalar_frame_init(" not in accepted.stdout
        assert "define i32 @oreslang_rt_scalar_frame_step(" not in accepted.stdout
        assert "fuel.remaining.logical" in accepted.stdout, "missing committed lifetime fuel debit"
        assert "fuel.logical.exhausted" in accepted.stdout, "missing immutable quota denial"
        verify_closed_guest_ir(accepted.stdout)
        llvm_ir = out / "guest.ll"
        llvm_ir.write_text(accepted.stdout)
        for optimization in ("-O0", "-O2", "-O3"):
            exe = out / f"run-guest-{optimization[2:]}"
            build = run([args.clang, "-std=c11", optimization, "-DNDEBUG",
                         "-Wall", "-Wextra", "-Werror",
                         "-Wno-error=override-module", "-pedantic",
                         str(args.harness), "-x", "ir", str(llvm_ir),
                         "-o", str(exe)])
            assert build.returncode == 0, (optimization, build.stderr)
            execution = run([str(exe)])
            assert execution.returncode == 0, (optimization, execution.stderr)
            assert "SCALAR_FRAME_AOT_TWO_TASKS_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_THREE_PEERS_BOUNDED_PROGRESS_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_CANCELLED_PEER_ISOLATION_PASS" in execution.stdout
            assert "SCALAR_FRAME_AOT_LOGICAL_FUEL_CONSERVATION_PASS" in execution.stdout
        for i, (original, replacement) in enumerate(cases):
            assert original in source
            file = out / f"invalid-{i}.ores"
            file.write_text(source.replace(original, replacement, 1))
            wrong = compile_source(file)
            assert wrong.returncode != 0 and not wrong.stdout.strip(), (i, wrong.stdout)
        missing = run([compiler, "--experimental-rt-scalar-loop", "--target=wasm32", str(fixture)])
        assert missing.returncode != 0 and not missing.stdout.strip()
        print("NATIVE_OPT_IN_SCALAR_FRAME_PASS: O0/O2/O3, 3 peers, conserved logical fuel, cancellation, negative traps")

if __name__ == "__main__":
    # This legacy harness still has many assertion-based end-to-end checks.
    # Never report a false PASS when PYTHONOPTIMIZE/-O would disable them.
    if sys.flags.optimize:
        raise SystemExit("native AOT security tests require Python assertions enabled")
    main()
