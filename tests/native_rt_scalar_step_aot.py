#!/usr/bin/env python3
"""Compile actual opt-in Oreslang source and drive two native heap continuations."""
import argparse
import pathlib
import re
import subprocess
import tempfile

def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, check=False,
                          timeout=60, **kwargs)

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
    assert definitions == expected, ("unapproved compiled guest function", definitions)
    declarations = set(re.findall(r'(?m)^declare\s+[^@\n]+@([A-Za-z0-9_.]+)\(', source))
    assert declarations == {"llvm.trap"}, ("unapproved guest import or FFI", declarations)
    assert not re.search(r'(?m)^\s*(?:%[\w.]+\s*=\s*)?(?:invoke|callbr)\b',
                         source), "unverified interprocedural transfer"
    calls = re.findall(
        r'(?m)^\s*(?:%[\w.]+\s*=\s*)?(?:(?:tail|musttail|notail)\s+)?call\b[^\n]*',
        source,
    )
    for call in calls:
        assert re.search(r'@llvm\.trap\(', call), ("unapproved guest call", call)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--fixture", required=True)
    p.add_argument("--harness", required=True)
    args = p.parse_args()
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
    main()
