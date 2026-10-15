#!/usr/bin/env python3
"""Run two REAL Oreslang compiler smoke fixtures. No mocks or shell.

The report attests observations of this CI job, not reproducible binary provenance,
cross-backend parity, or general language support. Compiler processes run with
normal runner privileges: sandbox/network isolation is the CI runner's duty.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
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


def git_checkout_sha(root):
    """Read the actual checked-out Git revision, not just CI's claimed SHA."""
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--verify", "HEAD"],
        capture_output=True, text=True, check=True, timeout=10,
    )
    revision = completed.stdout.strip()
    if not SOURCE_SHA.fullmatch(revision):
        raise ValueError("checkout HEAD is not a full lowercase Git commit SHA")
    return revision


def _kill_process_group(proc):
    """Kill the dedicated process group; separately daemonized children may escape."""
    try:
        if os.name == "posix":
            os.killpg(proc.pid, signal.SIGKILL)
        else:
            proc.kill()
    except ProcessLookupError:
        pass


def run(argv, timeout, cwd):
    """Run an argv vector with bounded capture and bounded wall time.

    No shell. A dedicated POSIX process group permits stopping descendants
    if the compiler launcher (such as the Java Python probe) wedges.
    This is not an OS sandbox and cannot kill descendants that create new sessions.
    """
    start = time.monotonic()
    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    expired, truncated = False, False
    proc = None
    code = None
    error_text = ""
    try:
        proc = subprocess.Popen(
            argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            stdin=subprocess.DEVNULL, start_new_session=(os.name == "posix"),
        )
        with selectors.DefaultSelector() as selector:
            selector.register(proc.stdout, selectors.EVENT_READ, "stdout")
            selector.register(proc.stderr, selectors.EVENT_READ, "stderr")
            while selector.get_map():
                remaining = timeout - (time.monotonic() - start)
                if remaining <= 0:
                    expired = True
                    break
                for key, _ in selector.select(timeout=min(remaining, 0.25)):
                    chunk = os.read(key.fileobj.fileno(), 8192)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        key.fileobj.close()
                        continue
                    target = buffers[key.data]
                    room = MAX_OUTPUT - len(target)
                    target.extend(chunk[:max(0, room)])
                    if len(chunk) > room:
                        truncated = True
                        break
                if truncated:
                    break
        if not expired and not truncated:
            try:
                proc.wait(timeout=max(0.001, timeout - (time.monotonic() - start)))
            except subprocess.TimeoutExpired:
                expired = True
        if expired or truncated:
            _kill_process_group(proc)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            _kill_process_group(proc)
            proc.wait(timeout=3)
        code = proc.returncode
    except OSError as exc:
        error_text = str(exc)[:MAX_OUTPUT]
    finally:
        if proc is not None:
            for pipe in (proc.stdout, proc.stderr):
                if pipe and not pipe.closed:
                    pipe.close()
            if proc.poll() is None:
                _kill_process_group(proc)
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
    return {
        "argv": argv, "exit_code": code,
        "stdout": buffers["stdout"].decode("utf-8", errors="replace"),
        "stderr": (buffers["stderr"].decode("utf-8", errors="replace") + error_text)[:MAX_OUTPUT],
        "output_truncated": truncated, "timeout": expired,
        "duration_ms": int((time.monotonic() - start) * 1000),
    }


def good(result):
    return not result["timeout"] and not result["output_truncated"] and result["exit_code"] == 0


def rejected(result):
    """Require a real same-line compiler diagnostic naming the rejected symbol.

    Unrelated host errors + a separately echoed fixture cannot qualify.
    """
    text = result["stdout"] + "\n" + result["stderr"]
    diagnostic = any(
        "nonexistent_symbol" in line and ERROR_WORDS.search(line)
        for line in text.splitlines()
    )
    return (
        not result["timeout"] and not result["output_truncated"]
        and isinstance(result["exit_code"], int)
        and result["exit_code"] > 0
        and diagnostic
        and not re.search(r"\b(segmentation fault|stack trace|traceback|fatal signal)\b", text, re.I)
    )

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=["java", "llvm"], required=True)
    parser.add_argument("--compiler", required=True, help="Absolute Python process invoker for Java, or native LLVM compiler executable")
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
    try:
        checkout_sha = git_checkout_sha(root)
    except (OSError, ValueError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        parser.error("cannot independently read Git checkout HEAD: " + type(error).__name__)
    if checkout_sha != args.source_sha:
        parser.error("declared source revision does not match Git checkout HEAD")
    evidence = {
        "schema_version": 1,
        "evidence_type": "real-compiler-smoke",
        "backend": args.backend,
        "source_commit_declared": args.source_sha,
        "actual_git_checkout_sha": checkout_sha,
        "source_commit_binary_link_attested": False,
        "compiler_path": str(compiler),
        "process_invoker_sha256": sha256(compiler),
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
        evidence["compiler_artifact_sha256"] = sha256(clazz)
        evidence["compiler_artifact_kind"] = "java-launcher-class-only"
        evidence["maven_classpath_sha256"] = sha256(cp)
        evidence["java_probe_path"] = str(probe)
        evidence["java_probe_sha256"] = sha256(probe)
        compile_argv = lambda source: [str(compiler), str(probe), str(source)]
        run_argv = lambda source: [str(compiler), str(probe), str(source), "--execute"]
    else:
        if not args.clang:
            parser.error("--clang is required for native LLVM result validation")
        clang = Path(args.clang)
        if not clang.is_absolute() or not clang.is_file():
            parser.error("--clang must be an existing absolute file")
        evidence["clang_path"] = str(clang)
        evidence["clang_sha256"] = sha256(clang)
        evidence["compiler_artifact_sha256"] = sha256(compiler)
        evidence["compiler_artifact_kind"] = "llvm-native-executable"
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
