#!/usr/bin/env python3
"""Run two REAL Oreslang compiler smoke fixtures. No mocks, shell, or network.

The report attests observations of this CI job, not reproducible binary provenance,
cross-backend parity, or general language support.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

POSITIVE = "pub fnc main(): int {\n  return 20 + 22;\n}\n"
NEGATIVE = "pub fnc main(): int {\n  return nonexistent_symbol;\n}\n"
ERROR_WORDS = re.compile(r"\b(error|undefined|undeclared|unresolved|unknown)\b|not found|cannot find", re.I)
SOURCE_SHA = re.compile(r"^[0-9a-f]{40}$")
MAX_OUTPUT = 32768


def sha256(path):
    hasher = hashlib.sha256()
    with open(path, "rb") as file:
        while chunk := file.read(65536):
            hasher.update(chunk)
    return hasher.hexdigest()


def run(argv, timeout, cwd):
    start = time.monotonic()
    try:
        completed = subprocess.run(
            argv, cwd=cwd, capture_output=True, timeout=timeout, check=False,
        )
        raw_out = completed.stdout
        raw_err = completed.stderr
        overflow = len(raw_out) > MAX_OUTPUT or len(raw_err) > MAX_OUTPUT
        return {
            "argv": argv, "exit_code": completed.returncode,
            "stdout": raw_out[:MAX_OUTPUT].decode("utf-8", errors="replace"),
            "stderr": raw_err[:MAX_OUTPUT].decode("utf-8", errors="replace"),
            "output_truncated": overflow, "timeout": False,
            "duration_ms": int((time.monotonic() - start) * 1000),
        }
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {
            "argv": argv, "exit_code": None, "stdout": "", "stderr": str(exc)[:MAX_OUTPUT],
            "output_truncated": False,
            "timeout": isinstance(exc, subprocess.TimeoutExpired),
            "duration_ms": int((time.monotonic() - start) * 1000),
        }


def good(result):
    return not result["timeout"] and not result["output_truncated"] and result["exit_code"] == 0


def rejected(result):
    text = result["stdout"] + "\n" + result["stderr"]
    return (not result["timeout"] and not result["output_truncated"]
            and isinstance(result["exit_code"], int)
            and result["exit_code"] > 0
            and "nonexistent_symbol" in text
            and bool(ERROR_WORDS.search(text))
            and not re.search(r"\b(segmentation fault|stack trace|traceback|fatal signal)\b", text, re.I))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=["java", "llvm"], required=True)
    parser.add_argument("--compiler", required=True, help="Absolute Java probe executable (Python), or LLVM compiler executable")
    parser.add_argument("--java-probe", help="Absolute path to reviewed java_probe.py, required for Java")
    parser.add_argument("--clang", help="Absolute clang path, required for LLVM native execution")
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()

    if not SOURCE_SHA.fullmatch(args.source_sha):
        parser.error("--source-sha must be a complete lowercase Git commit SHA")
    compiler = Path(args.compiler)
    if not compiler.is_absolute() or not compiler.is_file():
        parser.error("--compiler must name an existing absolute file")
    root = Path(__file__).resolve().parents[1]
    evidence = {
        "schema_version": 1,
        "evidence_type": "real-compiler-smoke",
        "backend": args.backend,
        "source_commit_declared": args.source_sha,
        "source_commit_binary_link_attested": False,
        "compiler_path": str(compiler),
        "compiler_sha256": sha256(compiler),
        "platform": sys.platform,
        "runner_id": os.environ.get("GITHUB_RUN_ID"),
        "runner_sha": os.environ.get("GITHUB_SHA"),
        "compiler_verification": "real-process-observation-only",
        "cases": [],
        "ok": False,
    }
    # GitHub's checkout SHA must match the source identity claimed in its own CI.
    if evidence["runner_sha"] and evidence["runner_sha"] != args.source_sha:
        parser.error("source SHA differs from GITHUB_SHA; refuse to attest mismatched revision")
    if args.backend == "java":
        if not args.java_probe:
            parser.error("--java-probe is required for Java")
        probe = Path(args.java_probe)
        if not probe.is_absolute() or not probe.is_file():
            parser.error("--java-probe must be an existing absolute file")
        clazz = root / "target/classes/dev/oreslang/launcher/OresMain.class"
        cp = root / "target/conformance-classpath.txt"
        if not clazz.is_file() or not cp.is_file() or not cp.read_text().strip():
            parser.error("real Java launcher classes and Maven classpath must be built first")
        evidence["launcher_class_sha256"] = sha256(clazz)
        evidence["maven_classpath_sha256"] = sha256(cp)
        evidence["java_probe_sha256"] = sha256(probe)
        compile_argv = lambda source: [str(compiler), str(probe), str(source)]
        run_argv = lambda source: [str(compiler), str(probe), str(source), "--execute"]
    else:
        if not args.clang:
            parser.error("--clang is required for native LLVM result validation")
        clang = Path(args.clang)
        if not clang.is_absolute() or not clang.is_file():
            parser.error("--clang must be an existing absolute file")
        evidence["clang_sha256"] = sha256(clang)
        compile_argv = lambda source: [str(compiler), str(source)]

    with tempfile.TemporaryDirectory(prefix="ores-ai-smoke-") as directory:
        temp = Path(directory)
        sources = {"positive": POSITIVE, "negative": NEGATIVE}
        for label, source_text in sources.items():
            path = temp / (label + ".ores")
            path.write_text(source_text)
            source_hash = sha256(path)
            result = run(compile_argv(path), 90, root)
            case = {"id": label, "source_sha256": source_hash, "compile": result}
            if label == "negative":
                case["passed"] = rejected(result)
            else:
                case["passed"] = good(result)
                if case["passed"]:
                    if args.backend == "java":
                        executed = run(run_argv(path), 90, root)
                        case["execution"] = executed
                        case["passed"] = (
                            not executed["timeout"] and not executed["output_truncated"]
                            and executed["exit_code"] == 42
                        )
                    else:
                        # Compiler must emit real textual LLVM IR, not a success string.
                        ir_text = result["stdout"]
                        ir_file = temp / "app.ll"
                        if "define " not in ir_text or "main" not in ir_text:
                            case["passed"] = False
                        else:
                            ir_file.write_text(ir_text)
                            binary = temp / "app"
                            linked = run([str(clang), "-x", "ir", str(ir_file), "-o", str(binary)], 90, root)
                            case["link"] = linked
                            if good(linked):
                                executed = run([str(binary)], 10, root)
                                case["execution"] = executed
                                case["passed"] = (
                                    not executed["timeout"] and not executed["output_truncated"]
                                    and executed["exit_code"] == 42
                                )
                            else:
                                case["passed"] = False
            evidence["cases"].append(case)

    evidence["ok"] = all(item["passed"] for item in evidence["cases"]) and len(evidence["cases"]) == 2
    output = Path(args.report)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps({
        "backend": args.backend, "ok": evidence["ok"],
        "positive": evidence["cases"][0]["passed"],
        "negative": evidence["cases"][1]["passed"],
        "report": str(output),
        "claim": "real process results, not backend parity or full compiler coverage",
    }))
    return 0 if evidence["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
