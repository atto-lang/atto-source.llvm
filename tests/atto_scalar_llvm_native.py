#!/usr/bin/env python3
"""Exact-source LLVM compiler -> LLVM IR -> native Clang -> full i64 stdout.

This is an executable test candidate, not an authenticated Actions receipt.
Native OS exit status cannot represent signed i64; link an independent C
observer to the compiler's unchanged __oreslang_guest_main function.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BLOB = "43c4d295a237e483f70722bd90ec10d273b73854"
C_MAIN = (
    '#include <inttypes.h>\n#include <stdint.h>\n#include <stdio.h>\n'
    'extern int64_t __oreslang_guest_main(void);\n'
    'int main(void) {\n'
    '  const int64_t observed = __oreslang_guest_main();\n'
    '  if (printf("%" PRId64 "\\n", observed) < 0) return 1;\n'
    '  return fflush(stdout) == 0 ? 0 : 1;\n'
    '}\n'
)
LIMIT = 4 * 1024 * 1024


def require(ok, explanation):
    if not ok:
        raise RuntimeError(explanation)


def git_blob(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") +
                        b"\0" + data).hexdigest()


def no_duplicate_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate scalar manifest JSON key")
        result[key] = value
    return result


def regular(root, relative):
    require(isinstance(relative, str) and
            relative.startswith("contracts/atto-scalar-v1/") and
            "\\" not in relative and
            all(piece not in ("", ".", "..") for piece in relative.split("/")),
            "unsafe scalar source path")
    path = root
    for segment in relative.split("/"):
        path = path / segment
        require(not path.is_symlink(), "symlinked source or ancestor")
    require(path.is_file(), "missing/nonregular source")
    return path


def load_corpus(root=ROOT):
    manifest = regular(root, "contracts/atto-scalar-v1/cases.json").read_bytes()
    require(git_blob(manifest) == BLOB, "unreviewed Atto scalar manifest")
    doc = json.loads(manifest.decode("utf-8"), object_pairs_hook=no_duplicate_pairs)
    require(manifest == (json.dumps(doc, indent=2, ensure_ascii=False) +
                         "\n").encode("utf-8"), "noncanonical JSON manifest")
    require(doc.get("schema") == "attolang.scalar-core.v1" and
            doc.get("source_extension") == ".atto" and
            doc.get("compiler_routes") == ["java", "llvm", "wasm", "net", "forge"] and
            doc.get("evidence_status") == "NOT_EXECUTED",
            "incorrect scalar parity policy")
    cases = doc.get("cases")
    require(isinstance(cases, list) and len(cases) == 19,
            "incomplete 19-case Atto corpus")
    positive = negative = 0
    sources = []
    for case in cases:
        require(case.get("kind") in ("positive", "negative") and
                re.fullmatch("[a-z][a-z0-9-]*", case.get("id", "")),
                "invalid scalar kind/id")
        path = ("contracts/atto-scalar-v1/" + case["kind"] + "/" +
                case["id"] + ".atto")
        require(path == case.get("path"), "noncanonical case file mapping")
        data = regular(root, path).read_bytes()
        require(0 < len(data) <= 8192 and
                git_blob(data) == case.get("git_blob_sha1"),
                "mismatched scalar fixture source bytes: " + case["id"])
        if case["kind"] == "positive":
            positive += 1
            expected = case.get("expected_i64")
            require(isinstance(expected, str) and
                    re.fullmatch("-?(0|[1-9][0-9]*)", expected) and
                    -(1 << 63) <= int(expected) < 1 << 63,
                    "invalid typed i64 expectation")
        else:
            negative += 1
            require(case.get("diagnostic_family") in
                    ("checked-overflow", "immutable-binding", "removed-operator"),
                    "invalid negative diagnostic family")
        sources.append((case, data))
    require(positive == 10 and negative == 9,
            "missing Atto positive/negative cases")
    return sources


def execute(argv, cwd, seconds=25):
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        result = subprocess.run(argv, cwd=cwd, stdin=subprocess.DEVNULL,
                                stdout=out, stderr=err, timeout=seconds,
                                check=False)
        out.seek(0, 2)
        err.seek(0, 2)
        require(out.tell() <= LIMIT and err.tell() <= LIMIT,
                "compiler/native guest exceeded 4MiB output budget")
        out.seek(0)
        err.seek(0)
        return result.returncode, out.read(), err.read()


def native_ir(ir):
    # Host wrapper must exist exactly once. Rewrite ONLY its symbol for the
    # test C observer; never rewrite the actual i64 guest function or result.
    old = b"define i32 @main("
    require(ir.count(old) == 1 and
            ir.count(b"define i64 @__oreslang_guest_main(") == 1,
            "expected source-owned i64 guest and one native i32 wrapper")
    require(ir.count(b"@main(") == 1,
            "unexpected LLVM references to the OS exit-code wrapper")
    return ir.replace(old, b"define i32 @__atto_observed_os_wrapper(", 1)


def link_native(clang, compiled_ir, work):
    ir = work / "guest.ll"
    obj = work / "guest.o"
    observer = work / "observer.c"
    binary = work / "observed"
    ir.write_bytes(native_ir(compiled_ir))
    observer.write_text(C_MAIN, encoding="ascii")
    rc, out, err = execute([str(clang), "-O0", "-Wno-override-module",
                             "-x", "ir", str(ir), "-c", "-o", str(obj)], work)
    require(rc == 0 and obj.is_file() and obj.stat().st_size > 0,
            "Clang rejected compiler-emitted LLVM IR: " + repr(err[:512]))
    rc, out, err = execute([str(clang), "-std=c11", "-O0", "-Wall",
                             "-Wextra", "-Werror", str(observer), str(obj),
                             "-o", str(binary)], work)
    require(rc == 0 and binary.is_file() and binary.stat().st_size > 0,
            "Clang could not link the native i64 observer: " + repr(err[:512]))
    return binary


def check(compiler, clang):
    require(compiler.is_file() and clang.is_file(),
            "required LLVM native compiler/Clang unavailable")
    require(not compiler.is_symlink(),
            "refuse symlinked build compiler for exact-head proof")
    observations = []
    data = load_corpus()
    compiler_sha = hashlib.sha256(compiler.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="atto-llvm-i64-") as temporary:
        work = Path(temporary)
        for case, bytes_ in data:
            src = work / (case["id"] + ".atto")
            src.write_bytes(bytes_)
            rc, ir, diagnostic = execute([str(compiler), str(src)], work)
            kind = case["kind"]
            if kind == "positive":
                require(rc == 0 and ir.startswith(b"; ModuleID") and
                        diagnostic == b"",
                        "positive compiler failed: " + case["id"] +
                        " " + repr(diagnostic[:512]))
                native = link_native(clang, ir, work)
                rc, observed, stderr = execute([str(native)], work, 10)
                require((rc, observed, stderr) ==
                        (0, (case["expected_i64"] + "\n").encode(), b""),
                        "full signed-i64 native execution diverged: " + case["id"] +
                        " " + repr((rc, observed[:80], stderr[:80])))
                observations.append({"case": case["id"],
                    "typed_i64": case["expected_i64"], "mode": "native-linked-i64",
                    "source_git_blob": case["git_blob_sha1"],
                    "llvm_ir_sha256": hashlib.sha256(ir).hexdigest(),
                    "native_sha256": hashlib.sha256(native.read_bytes()).hexdigest()})
            else:
                family = case["diagnostic_family"]
                if family in ("removed-operator", "immutable-binding"):
                    marker = (b"ATTO_REMOVED_OPERATOR" if family == "removed-operator"
                              else b"assignment requires an unloaned mutable scalar slot")
                    require(rc == 1 and ir == b"" and marker in diagnostic,
                            "invalid source passed compiler admission: " + case["id"])
                elif rc == 0:
                    require(ir.startswith(b"; ModuleID") and diagnostic == b"",
                            "overflow emitted malformed LLVM IR: " + case["id"])
                    native = link_native(clang, ir, work)
                    rc_guest, stdout, stderr = execute([str(native)], work, 10)
                    require(rc_guest != 0 and stdout == b"",
                            "checked i64 overflow returned a normal result: " + case["id"])
                else:
                    require(rc == 1 and ir == b"" and
                            b"overflow" in diagnostic.lower(),
                            "overflow rejected without compiler-owned overflow diagnostic: " +
                            case["id"])
        require(len(observations) == 10,
                "missing native full i64 positive observations")
    require(hashlib.sha256(compiler.read_bytes()).hexdigest() == compiler_sha,
            "compiler mutated during full-i64 verification")
    print(json.dumps({"status": "LLVM_NATIVE_I64_EXECUTED_LOCALLY",
        "compiled_guest_positives": 10, "negative_sources_checked": 9,
        "source_only": False, "independently_verified": False,
        "compiler_sha256": compiler_sha, "observations": observations},
        sort_keys=True))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--clang", type=Path, required=True)
    args = p.parse_args()
    check(args.compiler.resolve(strict=True), args.clang.resolve(strict=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print("ATTO_LLVM_SCALAR_NATIVE_GATE_DENIED: " + str(exc), file=sys.stderr)
        sys.exit(1)
