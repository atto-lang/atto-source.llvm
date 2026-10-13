#!/usr/bin/env python3
"""Closed-world LLVM AOT admission: unsupported attributes/macros must fail without IR."""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

BASE = "pub fnc main(): int { return 42; }\n"
NEGATIVE_CASES = {
    "function_attribute": "@Deprecated\n" + BASE,
    "parameter_attribute": "pub fnc main(@Range(0, 1) int n): int { return n; }\n",
    "native_layout_attribute": "@repr(c)\ndefine struct Point as x: int; end\n" + BASE,
    "derive_attribute": "@derive(Eq, Hash)\ndefine struct Point as x: int; end\n" + BASE,
    "actor_attribute": "@ActorMessage\ndefine actor Worker as end\n" + BASE,
    "aot_macro": "@expand(aot)\nmacro_rules! id { ($x:expr) => { $x }; }\n" + BASE,
    "rebuildable_macro": "@expand(rebuildable)\nmacro_rules! id { ($x:expr) => { $x }; }\n" + BASE,
    "jit_macro": "@expand(jit)\nmacro_rules! id { ($x:expr) => { $x }; }\n" + BASE,
    "unannotated_macro": "macro_rules! id { ($x:expr) => { $x }; }\n" + BASE,
    "inline_annotation": "@inline(always)\n" + BASE,
    "conditional_compilation": "@cfg(feature)\n" + BASE,
    "alignment_attribute": "@align(64)\n" + BASE,
    "actor_state_capability": "@RequiresState(write)\n" + BASE,
    "generated_builder": "@GenerateBuilder\n" + BASE,
    "generated_json_accessor": "@FromJson(\"id\")\n" + BASE,
    "misplaced_expand": "@expand(aot)\n" + BASE,
    "trailing_attribute_after_valid_program": BASE + "@derive(Eq)\n",
    "trailing_macro_after_valid_program": BASE + "macro_rules! x { () => { 42 }; }\n",
}


def invoke(compiler, directory, case, source, target=None):
    # Give every fixture an isolated working directory. A reject result may
    # emit an .ll, .bc or binary file without printing IR to stdout: that
    # violates closed-world AOT admission even when the exit status is nonzero.
    workdir = pathlib.Path(directory) / case
    workdir.mkdir()
    path = workdir / "input.ores"
    path.write_text(source, encoding="utf-8")
    # Absolute compiler paths and executables resolved through PATH both work
    # after changing the subprocess working directory to its isolated fixture.
    executable = shutil.which(compiler)
    if executable is None:
        raise FileNotFoundError("LLVM compiler is not executable: " + compiler)
    command = [str(pathlib.Path(executable).resolve())]
    if target:
        command.append("--target=" + target)
    command.append(str(path))
    result = subprocess.run(command, cwd=workdir,
                            capture_output=True, text=True, check=False, timeout=15)
    emitted = sorted(str(item.relative_to(workdir))
                     for item in workdir.rglob("*")
                     if item.is_file() and item != path)
    return result, emitted


def execute_host_ir(clang, directory, case, ir):
    """Link actual compiler-produced IR and execute the guest artifact, no JIT."""
    workdir = pathlib.Path(directory) / ("native_" + case)
    workdir.mkdir()
    ll = workdir / "guest.ll"
    executable = workdir / "guest"
    ll.write_text(ir, encoding="utf-8")
    tool = shutil.which(clang)
    if tool is None:
        return "Clang executable not found: " + clang
    linked = subprocess.run(
        [str(pathlib.Path(tool).resolve()), "-x", "ir", str(ll),
         "-o", str(executable)],
        cwd=workdir, capture_output=True, text=True, timeout=30, check=False)
    if linked.returncode or not executable.is_file():
        return "Clang failed to link guest IR: " + linked.stderr[:400]
    executed = subprocess.run(
        [str(executable)], cwd=workdir, capture_output=True,
        text=True, timeout=15, check=False)
    if executed.returncode != 42 or executed.stdout.strip() or executed.stderr.strip():
        return ("native guest expected exit 42 and no output; got exit "
                + str(executed.returncode) + ", stdout=" + repr(executed.stdout[:160])
                + ", stderr=" + repr(executed.stderr[:160]))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--clang", required=True)
    args = ap.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="oreslang-aot-attributes-") as directory:
        for target in ("host", "wasm32"):
            good, _good_files = invoke(args.compiler, directory, "control_" + target,
                          "// @derive(Eq) in comments is not active metadata\n"
                          + "/* macro_rules! fake { () => { 42 }; } */\n" + BASE,
                          target)
            if good.returncode or "define i64 @main" not in good.stdout:
                failures.append("positive control failed for " + target +
                                ": " + good.stderr[:350])
            if _good_files:
                failures.append("positive control wrote unexpected files for " + target
                                + ": " + ", ".join(_good_files))
            if target == "wasm32" and 'target triple = "wasm32-unknown-unknown"' not in good.stdout:
                failures.append("wasm32 positive control omitted certified target triple")

            if target == "host" and good.returncode == 0:
                error = execute_host_ir(args.clang, directory, "simple_main", good.stdout)
                if error:
                    failures.append("host standalone guest AOT: " + error)

            # Positive native baseline: valid, unannotated typed calls must
            # remain compilable while unsupported metadata fails closed.
            calls, call_files = invoke(args.compiler, directory,
                                       "typed_calls_" + target,
                                       "pub fnc add(int a, int b): int { return a + b; }\n"
                                       "pub fnc main(): int { return add(20, 22); }\n",
                                       target)
            if calls.returncode or "define i64 @main" not in calls.stdout or "define i64 @add" not in calls.stdout:
                failures.append("typed call control failed for " + target
                                + ": " + calls.stderr[:350])
            if call_files:
                failures.append("typed call control wrote unexpected files for " + target
                                + ": " + ", ".join(call_files))
            if target == "host" and calls.returncode == 0:
                error = execute_host_ir(args.clang, directory, "typed_calls", calls.stdout)
                if error:
                    failures.append("host typed-call guest AOT: " + error)
            for label, source in NEGATIVE_CASES.items():
                result, emitted = invoke(args.compiler, directory, label + "_" + target, source, target)
                if result.returncode == 0:
                    failures.append(label + "/" + target + " accepted unsupported syntax")
                if result.stdout.strip():
                    failures.append(label + "/" + target + " emitted IR on rejected source")
                if emitted:
                    failures.append(label + "/" + target
                                    + " wrote artifacts on rejected source: " + ", ".join(emitted))
                if not result.stderr.strip():
                    failures.append(label + "/" + target + " gave no rejection diagnostic")
    if failures:
        for failure in failures:
            print("FAIL:", failure, file=sys.stderr)
        return 1
    print("PASS: LLVM host guest-AOT executed twice; host/wasm32 annotation/macro admission enforced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
