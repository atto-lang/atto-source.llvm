#!/usr/bin/env python3
"""Actual JVM/Native Image guest conformance (not an admission-only probe).

Use the Java source repo's real contracts/v1/java_probe.py on the JVM, or a
real GraalVM native binary with --build-info proof. Pending cases are measured
but never silently promoted to required. No source rewriting or fake exits.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "contracts/v1/generic-scalar"
sys.path.insert(0, str(CONTRACT))
from verify import load_suite  # noqa: E402


def run_command(args):
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        try:
            process = subprocess.run(args, stdout=stdout, stderr=stderr,
                                     timeout=100, check=False)
        except subprocess.TimeoutExpired:
            return {"rc": -999, "stdout": "", "stderr": "timed out"}
        stdout.seek(0, 2)
        stderr.seek(0, 2)
        if stdout.tell() > 262144 or stderr.tell() > 262144:
            return {"rc": -998, "stdout": "", "stderr": "output exceeds 256 KiB"}
        stdout.seek(0)
        stderr.seek(0)
        return {"rc": process.returncode,
                "stdout": stdout.read().decode("utf-8", "replace"),
                "stderr": stderr.read().decode("utf-8", "replace")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--backend", choices=("jvm", "native-image"), required=True)
    ap.add_argument("--java-probe", type=Path)
    ap.add_argument("--native-binary", type=Path)
    ap.add_argument("--report", type=Path)
    args = ap.parse_args()
    if args.backend == "jvm":
        if not args.java_probe or args.native_binary:
            ap.error("JVM requires only --java-probe")
        launcher = args.java_probe.resolve(strict=True)
        def command(src, check):
            return [sys.executable, str(launcher), str(src)] + ([] if check else ["--execute"])
    else:
        if not args.native_binary or args.java_probe:
            ap.error("Native Image requires only --native-binary")
        launcher = args.native_binary.resolve(strict=True)
        evidence = run_command([str(launcher), "--build-info"])
        if evidence["rc"] != 0 or '"build_mode":"aot"' not in evidence["stdout"]:
            raise RuntimeError("not a proven Oreslang Native Image AOT binary")
        def command(src, check):
            return [str(launcher), "--mode=aot"] + (["--check"] if check else []) + [str(src)]
    cases, digest = load_suite()
    results = []
    required_failures = 0
    for case in cases:
        source = (CONTRACT / case["source"]).resolve(strict=True)
        required = case["backends"]["graal"] == "required"
        checked = run_command(command(source, True))
        result = {"id": case["id"], "required": required, "kind": case["kind"],
                  "check_exit": checked["rc"]}
        if case["kind"] == "native_exit":
            executed = run_command(command(source, False)) if checked["rc"] == 0 else None
            result["guest_exit"] = executed["rc"] if executed else None
            accepted = checked["rc"] == 0 and bool(executed) and executed["rc"] == case["expected_exit"]
            result["diagnostic"] = (checked["stderr"] + (executed or {}).get("stderr", ""))[-1024:]
        else:
            actual_diagnostic = checked["stderr"] + checked["stdout"]
            marker = case.get("diagnostics", {}).get("graal")
            accepted = checked["rc"] > 0 and (not required or bool(marker and marker in actual_diagnostic))
            result["diagnostic"] = actual_diagnostic[-1024:]
            result["expected_marker_match"] = bool(marker and marker in actual_diagnostic)
        result["passed"] = bool(accepted)
        if not accepted and required:
            required_failures += 1
        results.append(result)
        print(f"{'PASS' if accepted else 'OBSERVED_MISMATCH' if not required else 'FAIL'} "
              f"{args.backend}: {case['id']} ({'required' if required else 'pending'}) "
              f"check={checked['rc']} guest={result.get('guest_exit', '-')}", flush=True)
    verdict = {"backend": args.backend, "fixture_suite_sha256": digest,
               "required_failures": required_failures, "results": results}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8")
    print(f"{args.backend}: required_pass={sum(r['passed'] for r in results if r['required'])}, "
          f"required_fail={required_failures}, pending={sum(not r['required'] for r in results)}",
          flush=True)
    return 1 if required_failures else 0


if __name__ == "__main__":
    sys.exit(main())
