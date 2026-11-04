#!/usr/bin/env python3
"""Real LLVM .atto Option/Result guest parity at Clang O0 and O2.

Requires a source-built oreslang-llvmc and actual Clang native processes.
This is not synthetic IR, a parser-only canary, or a host-only resolver test.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

POSITIVES = (
    ("native-sum-option-bool-atto.atto", 42),
    ("native-sum-result-string-atto.atto", 42),
    ("native-sum-option-none-atto.atto", 42),
    ("native-sum-result-ok-atto.atto", 42),
)
NEGATIVES = (
    ("native-sum-option-bool-invalid-atto.atto",
     "native sum constructor payload type does not match declared variant"),
    ("native-sum-result-string-invalid-atto.atto",
     "native sum constructor payload type does not match declared variant"),
    ("native-sum-option-bool-conditional-invalid-atto.atto",
     "native sum conditional branch payload types do not match"),
    ("native-sum-result-string-conditional-invalid-atto.atto",
     "native sum conditional branch payload types do not match"),
    ("native-sum-option-bool-fallback-invalid-atto.atto",
     "native sum unwrap_or fallback type does not match payload"),
    ("native-sum-result-fallback-invalid-atto.atto",
     "native sum unwrap_or fallback type does not match payload"),
    ("native-sum-result-ok-invalid-atto.atto",
     "native sum constructor payload type does not match declared variant"),
    ("native-sum-option-none-arity-invalid-atto.atto",
     "native sum constructor requires its exact positional payload arity"),
)

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def run(command, *, stdin=None, timeout=60, env=None):
    return subprocess.run(command, input=stdin, capture_output=True,
                          check=False, timeout=timeout, env=env)

def sha(data):
    return hashlib.sha256(data).hexdigest()

NO_GC_SUCCESS = (
    b"STRICT_NO_GC_IR_SCREENED_ONLY "
    b"(linked binary and FFI audit still required)\n"
)


def check_no_gc(auditor, ir_path, original):
    # Source-owned CI builds the genuine Rust auditor in the CMake build tree.
    # The CLI must reject malformed success transcripts and detect mutation
    # of the auditor or checked IR during the invocation. Name/marker alone
    # are not an independent provenance attestation.
    require(auditor.name == "oreslang-no-gc-ir-audit" and
            auditor.is_file() and not auditor.is_symlink() and
            os.access(auditor, os.X_OK), "no-GC auditor missing or unsafe")
    before = auditor.stat()
    original_auditor = auditor.read_bytes()
    ir_path.write_bytes(original)
    screened = run([str(auditor), str(ir_path)], timeout=10)
    require(screened.returncode == 0 and
            screened.stdout == NO_GC_SUCCESS and
            not screened.stderr, "mandatory source-built no-GC audit failed")
    require(ir_path.read_bytes() == original, "no-GC audit modified compiler IR")
    after = auditor.stat()
    require(not auditor.is_symlink() and
            (before.st_dev, before.st_ino, before.st_mode, before.st_size,
             before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_mode, after.st_size,
             after.st_mtime_ns) and
            auditor.read_bytes() == original_auditor,
            "no-GC auditor changed during IR audit")


def self_test():
    # Synthetic SOURCE-ONLY invocation checks, NOT guest/compiler evidence.
    # Negative failures must be attributable to the expected control, never
    # to an unrelated Python or shell failure.
    with tempfile.TemporaryDirectory(prefix="atto-auditor-selftest-") as td:
        root = Path(td)
        ir_file = root / "sample.ll"
        sample = b"; ModuleID = 'synthetic'\n"
        success_line = NO_GC_SUCCESS.decode("ascii").strip()
        cases = (
            ("missing", None, "no-GC auditor missing or unsafe"),
            ("denied", "#!/bin/sh\nexit 17\n",
             "mandatory source-built no-GC audit failed"),
            ("tamper", '#!/bin/sh\nprintf changed >> "$1"\n'
             'echo "' + success_line + '"\n',
             "no-GC audit modified compiler IR"),
            ("marker-prefix", "#!/bin/sh\n"
             'echo "NOT_A_PROOF ' + success_line + '"\n',
             "mandatory source-built no-GC audit failed"),
            ("marker-suffix", "#!/bin/sh\n"
             'echo "' + success_line + ' FAKE"\n',
             "mandatory source-built no-GC audit failed"),
            ("no-marker", "#!/bin/sh\nexit 0\n",
             "mandatory source-built no-GC audit failed"),
            ("stderr", "#!/bin/sh\n"
             'echo "' + success_line + '"\n'
             'echo UNEXPECTED_STDERR >&2\n',
             "mandatory source-built no-GC audit failed"),
            ("self-modifying", "#!/bin/sh\n"
             'printf "# changed\\n" >> "$0"\n'
             'echo "' + success_line + '"\n',
             "no-GC auditor changed during IR audit"),
        )
        negative_count = 0
        for name, body, diagnostic in cases:
            auditor = root / name / "oreslang-no-gc-ir-audit"
            if body is not None:
                auditor.parent.mkdir()
                auditor.write_text(body)
                auditor.chmod(0o700)
            try:
                check_no_gc(auditor, ir_file, sample)
            except RuntimeError as error:
                require(diagnostic in str(error),
                        "auditor negative " + name +
                        " failed for the wrong reason: " + str(error))
                negative_count += 1
            else:
                raise RuntimeError("unsafe " + name + " auditor was accepted")
        passing = root / "pass" / "oreslang-no-gc-ir-audit"
        passing.parent.mkdir()
        passing.write_text("#!/bin/sh\n"
                           'echo "' + success_line + '"\n')
        passing.chmod(0o700)
        check_no_gc(passing, ir_file, sample)
        # A valid binary via a symlink remains untrusted even if it prints the
        # right success line. Do not resolve the symlink before validation.
        alias = root / "alias" / "oreslang-no-gc-ir-audit"
        alias.parent.mkdir()
        alias.symlink_to(passing)
        try:
            check_no_gc(alias, ir_file, sample)
        except RuntimeError as error:
            require("no-GC auditor missing or unsafe" in str(error),
                    "symlink auditor rejected for unexpected reason")
            negative_count += 1
        else:
            raise RuntimeError("symlink no-GC auditor was accepted")
        require(negative_count == len(cases) + 1,
                "auditor negative mutation coverage incomplete")
    print("NATIVE_SUM_AUDITOR_SOURCE_ONLY_PASS|negatives=" +
          str(negative_count) + "|guest=NOT_EXECUTED")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler")
    parser.add_argument("--clang")
    parser.add_argument("--root", type=Path)
    parser.add_argument("--auditor")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        require(not (args.compiler or args.clang or args.root or args.auditor),
                "self-test must not accept compiler-backed inputs")
        self_test()
        return
    require(all((args.compiler, args.clang, args.root, args.auditor)),
            "real compiler, Clang, root and source-built no-GC auditor required")
    root = args.root.resolve(strict=True)
    compiler = Path(args.compiler).resolve(strict=True)
    clang = Path(args.clang).resolve(strict=True)
    auditor_path = Path(args.auditor)
    require(not auditor_path.is_symlink(), "no-GC auditor symlink forbidden")
    auditor = auditor_path.resolve(strict=True)
    require(compiler.is_file() and compiler.name == "oreslang-llvmc",
            "native guest parity requires real oreslang-llvmc")
    require(clang.is_file(), "native guest parity requires real clang")
    fixtures = root / "tests" / "fixtures"
    regions = (root / "src/main/cpp/oreslang/runtime/NativeRecordRegion.c",
               root / "src/main/cpp/oreslang/runtime/NativeStringRegion.c")
    for region in regions:
        require(region.is_file() and not region.is_symlink(),
                "native region source missing or symlinked")
    count = 0
    production_count = 0
    all_receipts = []
    with tempfile.TemporaryDirectory(prefix="atto-native-sum-") as temp:
        temporary = Path(temp)
        # Real, source-built Rust auditor must reject known tracing collector IR.
        gc_ir = temporary / "forbidden-gc.ll"
        gc_ir.write_text("declare ptr @GC_malloc(i64)\n")
        gc_result = run([str(auditor), str(gc_ir)], timeout=10)
        require(gc_result.returncode != 0 and
                b"STRICT_NO_GC_IR_REJECTED" in gc_result.stderr,
                "auditor admitted known tracing-GC LLVM IR")
        for filename, expected in POSITIVES:
            source = fixtures / filename
            require(source.is_file() and not source.is_symlink() and
                    source.suffix == ".atto", "invalid positive .atto fixture")
            original = source.read_bytes()
            ir_hashes = []
            receipts = []
            for level in (0, 2):
                generated = run([str(compiler), "--target=host", str(source)])
                require(generated.returncode == 0 and
                        generated.stdout.startswith(b"; ModuleID") and
                        not generated.stderr,
                        "real frontend rejected positive " + filename)
                ir_hashes.append(sha(generated.stdout))
                ir_file = temporary / (filename + ".O" + str(level) + ".ll")
                check_no_gc(auditor, ir_file, generated.stdout)
                executable = temporary / (filename + ".O" + str(level))
                linked = run([str(clang), "-O" + str(level), "-x", "ir", "-",
                              "-x", "c", "-std=c11", "-pthread",
                              "-I", str(root / "src/main/cpp"),
                              *(str(region) for region in regions),
                              "-o", str(executable)], stdin=generated.stdout)
                require(linked.returncode == 0 and executable.is_file(),
                        "real Clang link failed for " + filename +
                        " O" + str(level))
                header = executable.read_bytes()[:4]
                if sys.platform == "darwin":
                    require(header in (b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                                       b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe"),
                            "native binary is not Mach-O on macOS")
                else:
                    require(header == b"\x7fELF",
                            "native binary is not ELF on Linux")
                native = run([str(executable)], timeout=10)
                require(native.returncode == expected,
                        "native guest returned wrong exit code: " + filename +
                        " O" + str(level))
                require(source.read_bytes() == original, "fixture mutated")
                receipts.append("NATIVE_ATTO_SUM_PASS|source=" + filename +
                                "|opt=O" + str(level) +
                                "|exit=" + str(expected) +
                                "|source_sha256=" + sha(original) +
                                "|ir_sha256=" + ir_hashes[-1] +
                                "|binary_sha256=" +
                                sha(executable.read_bytes()))
            require(ir_hashes[0] == ir_hashes[1],
                    "same source generated different frontend IR for O0/O2")
            # Run the current production AOT wrapper as an independent gate:
            # direct Clang comparisons alone do not qualify AOT publication.
            wrapper = root / "scripts" / "compile-aot.sh"
            require(wrapper.is_file() and not wrapper.is_symlink(),
                    "source-owned AOT publication wrapper missing")
            published = temporary / (filename + ".production")
            env = os.environ.copy()
            env.update({"ORESLANG_LLVMC": str(compiler),
                        "ORES_NO_GC_IR_AUDIT": str(auditor),
                        "CLANG": str(clang)})
            process = run(["bash", str(wrapper), str(source), str(published)],
                          timeout=60, env=env)
            require(process.returncode == 0 and published.is_file(),
                    "production AOT publisher failed for " + filename)
            native_header = published.read_bytes()[:4]
            if sys.platform == "darwin":
                require(native_header in
                        (b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                         b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe"),
                        "production AOT artifact is not Mach-O")
            else:
                require(native_header == b"\x7fELF",
                        "production AOT artifact is not ELF")
            actual = run([str(published)], timeout=10)
            require(actual.returncode == expected and
                    source.read_bytes() == original,
                    "production AOT guest failed or source mutated")
            all_receipts.extend(receipts)
            count += len(receipts)
            production_count += 1
            all_receipts.append(
                "NATIVE_ATTO_SUM_PRODUCTION_AOT_PASS|source=" + filename +
                "|exit=" + str(expected) +
                "|auditor_sha256=" + sha(auditor.read_bytes()) +
                "|binary_sha256=" + sha(published.read_bytes()))
        # Negative AOT publisher: a missing/failing auditor must leave an
        # existing output untouched; a compiler failure alone is not evidence.
        sentinel = temporary / "preexisting-aot"
        original_output = b"preexisting-binary-kept-on-audit-failure"
        failed_auditor = temporary / "failing-auditor"
        failed_auditor.write_text(
            "#!/bin/sh\necho AUDIT_DENIED_PROBE >&2\nexit 17\n")
        failed_auditor.chmod(0o700)
        for label, candidate in (("missing", temporary / "absent-auditor"),
                                 ("denied", failed_auditor)):
            sentinel.write_bytes(original_output)
            env = os.environ.copy()
            env.update({"ORESLANG_LLVMC": str(compiler),
                        "ORES_NO_GC_IR_AUDIT": str(candidate),
                        "CLANG": str(clang)})
            rejected = run(["bash", str(root / "scripts" / "compile-aot.sh"),
                            str(fixtures / POSITIVES[0][0]), str(sentinel)],
                           timeout=60, env=env)
            expected_marker = (b"Build oreslang-no-gc-ir-audit" if
                               label == "missing" else b"AUDIT_DENIED_PROBE")
            require(rejected.returncode != 0 and
                    expected_marker in rejected.stderr and
                    sentinel.read_bytes() == original_output,
                    "production AOT auditor " + label + " negative escaped")
        negatives = 0
        for filename, diagnostic in NEGATIVES:
            source = fixtures / filename
            require(source.is_file() and not source.is_symlink() and
                    source.suffix == ".atto", "invalid negative fixture")
            original = source.read_bytes()
            # Admission runs in oreslang-llvmc, before Clang optimization.
            # Retesting the same rejection twice is not an O0/O2 test.
            rejected = run([str(compiler), "--target=host", str(source)])
            require(rejected.returncode == 1, "bad source not rejected")
            require(not rejected.stdout, "failed compiler emitted LLVM IR")
            require(diagnostic.encode() in rejected.stderr,
                    "wrong rejection diagnostic for " + filename)
            require(source.read_bytes() == original, "negative source mutated")
            negatives += 1
    require(count == 2 * len(POSITIVES) and
            production_count == len(POSITIVES) and
            negatives == len(NEGATIVES) and
            len(all_receipts) == count + production_count,
            "native parity matrix incomplete")
    # Do not emit any PASS receipt if even the last negative fails.
    for receipt in all_receipts:
        print(receipt)
    print("NATIVE_ATTO_SUM_MATRIX_PASS|executed=" + str(count) +
          "|production_aot=" + str(production_count) +
          "|compiler_rejections=" + str(negatives) +
          "|no_gc_audited=YES|clang_levels=O0,O2|synthetic_ir=NO")

if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("native AttoLang typed-sum test failed: " + str(error),
              file=sys.stderr)
        sys.exit(1)
