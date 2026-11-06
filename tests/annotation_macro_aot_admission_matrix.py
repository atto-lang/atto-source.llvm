#!/usr/bin/env python3
"""Closed-world LLVM AOT admission: unsupported attributes/macros must fail without IR."""
import argparse
import base64
import binascii
import re
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
    "duplicate_return_contract": "@Ret(int) @Ret(bool)\n" + BASE,
    "return_annotation_on_module": "@Ret(int)\ndefine module demo as end\n",
    "field_annotation_on_module": "@FromJson(\"id\")\ndefine module demo as end\n",
    "unlowered_deadline": "@Critical(1)\n" + BASE,
    "permission_annotation": "@RequiresPermission(\"ffi\")\n" + BASE,
    "unknown_compiler_macro": "@UnrecognizedMacro\n" + BASE,
    "parallel_actor_annotation": "@ActorMaxThreads(MaxActorThreads.HIGH)\ndefine actor Worker as end\n" + BASE,
    "misplaced_expand": "@expand(aot)\n" + BASE,
    "trailing_attribute_after_valid_program": BASE + "@derive(Eq)\n",
    "trailing_macro_after_valid_program": BASE + "macro_rules! x { () => { 42 }; }\n",
}

# Candidate AttoLang v2 target inventory: every unsupported target remains a
# compiler-level rejection until typed AST/HIR retention, a resolved schema,
# and native AOT lowering are independently proved. These are negative
# admission fixtures, not a claim that their source grammars are implemented.
TARGET_ANNOTATION_CASES = {
    "module_target": "@deprecated\ndefine module demo as end\n",
    "class_target": "@deprecated\ndefine class Item as end\n" + BASE,
    "struct_target": "@derive(Eq)\ndefine struct Item as id: int; end\n" + BASE,
    "enum_target": "@serde(tag=kind)\ndefine enum Status as Ready; end\n" + BASE,
    "enum_variant_target": "define enum Status as @serde(name=ready) Ready; end\n" + BASE,
    "actor_target": "@actor_policy(serial=true)\ndefine actor Worker as end\n" + BASE,
    "isoactor_target": "@supervision(strategy=restart)\ndefine isoactor Worker as end\n" + BASE,
    "interface_target": "@deprecated\ndefine interface Readable as end\n" + BASE,
    "trait_target": "@deprecated\ndefine trait Readable as end\n" + BASE,
    "type_alias_target": "@deprecated\ndefine type Count = int;\n" + BASE,
    "fnc_target": "@must_use\n" + BASE,
    "routine_target": "@pure\nroutine answer(): int { return 42; }\n" + BASE,
    "method_target": "define class Item as @inline(always) read(): int { return 42; } end\n" + BASE,
    "constructor_target": "define class Item as @requires(Capability.Read) constructor() {} end\n" + BASE,
    "field_target": "define class Item as @serde(name=id) id: int; end\n" + BASE,
    "actor_state_target": "define actor Worker as @refine(min=0) private count: int; end\n" + BASE,
    "parameter_target": "pub fnc value(@borrow int x): int { return x; }\n" + BASE,
    "return_type_target": "pub fnc answer(): @refine(min=0) int { return 42; }\n" + BASE,
    "generic_parameter_target": "pub fnc id<@refine(min=0) T>(T x): T { return x; }\n" + BASE,
    "local_binding_target": "pub fnc main(): int { @refine(min=0) const int x = 42; return x; }\n",
    "type_use_target": "pub fnc value(@align(16) int x): int { return x; }\n" + BASE,
    "lambda_target": "pub fnc main(): int { const f = @pure |int x| -> x; return 42; }\n",
    "macro_target": "@derive_macro\nmacro_rules! make { () => { 42 }; }\n" + BASE,
    "import_target": "@cfg(feature=network)\nimport module network;\n" + BASE,
    "doc_class_unlowered": '@Doc("description")\ndefine class Item as end\n' + BASE,
    "doc_actor_unlowered": '@Doc("worker")\ndefine actor Worker as end\n' + BASE,
    "doc_method_unlowered": 'define class Item as @Doc("read") read(): int { return 1; } end\n' + BASE,
}


def shared_annotation_cases():
    """Schema-checked byte-identical corpus; expected index 2 is LLVM."""
    path = pathlib.Path(__file__).resolve().parents[1] / (
        "contracts/v1/annotation-admission-v1.cases")
    cases = []
    seen = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("|")
        if len(fields) != 7:
            raise ValueError("invalid annotation parity row schema")
        key, *expected, encoded = fields
        if not re.fullmatch(r"[a-z][a-z_0-9]*", key) or key in seen:
            raise ValueError("invalid or duplicate annotation parity id: " + key)
        seen.add(key)
        if any(value not in {"pass", "reject"} for value in expected):
            raise ValueError("invalid annotation parity expectation: " + key)
        try:
            raw = base64.b64decode(encoded, validate=True)
            source = raw.decode("utf-8", errors="strict")
        except (ValueError, UnicodeError, binascii.Error) as ex:
            raise ValueError("invalid annotation parity source: " + key) from ex
        if base64.b64encode(raw).decode("ascii") != encoded:
            raise ValueError("noncanonical source bytes: " + key)
        cases.append((key, source, expected[1]))
    if len(cases) != 17:
        raise ValueError("incomplete annotation parity corpus")
    return cases


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


def rejected_compiler_outcome(result, emitted, label):
    """Require compiler rejection, never crash, partial IR, or sidecar output."""
    errors = []
    # Subprocess return codes below zero mean signal death; 128+ commonly
    # encode signals/aborts and must never count as a successful rejection.
    if result.returncode <= 0 or result.returncode >= 128:
        errors.append(label + ": invalid compiler reject exit " + str(result.returncode))
    if result.stdout.strip():
        errors.append(label + ": rejected source emitted stdout/IR")
    if emitted:
        errors.append(label + ": rejected source emitted artifacts: " + ", ".join(emitted))
    if not result.stderr.strip():
        errors.append(label + ": missing compiler rejection diagnostic")
    if re.search(
        r"segmentation fault|internal compiler error|assertion failed|"
        r"stack backtrace|panicked at|traceback \\(most recent call last\\)|"
        r"fatal signal|bus error", result.stderr, re.IGNORECASE
    ):
        errors.append(label + ": compiler crash is not a semantic rejection")
    return errors


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
            # Native Oreslang main is i64 behind the checked i32 OS wrapper.
            # Wasm retains its guest i64 export without that process wrapper.
            main_signatures = (("define i64 @__oreslang_guest_main(", "define i32 @main(")
                               if target == "host" else ("define i64 @main(",))
            good, _good_files = invoke(args.compiler, directory, "control_" + target,
                          "// @derive(Eq) in comments is not active metadata\n"
                          + "/* macro_rules! fake { () => { 42 }; } */\n" + BASE,
                          target)
            if good.returncode or any(signature not in good.stdout for signature in main_signatures):
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
            if calls.returncode or any(signature not in calls.stdout for signature in main_signatures) or "define i64 @add" not in calls.stdout:
                failures.append("typed call control failed for " + target
                                + ": " + calls.stderr[:350])
            if call_files:
                failures.append("typed call control wrote unexpected files for " + target
                                + ": " + ", ".join(call_files))
            if target == "host" and calls.returncode == 0:
                error = execute_host_ir(args.clang, directory, "typed_calls", calls.stdout)
                if error:
                    failures.append("host typed-call guest AOT: " + error)
            for label, source, expectation in shared_annotation_cases():
                result, emitted = invoke(args.compiler, directory,
                                         "parity_" + label + "_" + target,
                                         source, target)
                if expectation == "reject":
                    failures.extend(rejected_compiler_outcome(
                        result, emitted, "annotation parity/" + label + "/" + target))
                else:
                    if result.returncode or any(sig not in result.stdout for sig in main_signatures) or emitted:
                        failures.append("annotation parity positive failed: " + label
                                        + "/" + target + " stderr=" + result.stderr[:250])
            # V2 candidate placement matrix: both native host and wasm32
            # must reject before emitting IR or a sidecar artifact.
            for label, source in TARGET_ANNOTATION_CASES.items():
                result, emitted = invoke(args.compiler, directory,
                                         "v2_" + label + "_" + target, source, target)
                failures.extend(rejected_compiler_outcome(
                    result, emitted, "v2/" + label + "/" + target))
            for label, source in NEGATIVE_CASES.items():
                result, emitted = invoke(args.compiler, directory, label + "_" + target, source, target)
                failures.extend(rejected_compiler_outcome(
                    result, emitted, label + "/" + target))
    if failures:
        for failure in failures:
            print("FAIL:", failure, file=sys.stderr)
        return 1
    print("PASS: LLVM host guest-AOT executed twice; host/wasm32 annotation/macro admission enforced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
