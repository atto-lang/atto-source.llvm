#!/usr/bin/env python3
"""Closed-world LLVM AOT admission: unsupported attributes/macros must fail without IR."""
import argparse
import pathlib
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
    "misplaced_expand": "@expand(aot)\n" + BASE,
    "trailing_attribute_after_valid_program": BASE + "@derive(Eq)\n",
    "trailing_macro_after_valid_program": BASE + "macro_rules! x { () => { 42 }; }\n",
}


def invoke(compiler, directory, case, source, target=None):
    path = pathlib.Path(directory) / (case + ".ores")
    path.write_text(source, encoding="utf-8")
    command = [compiler]
    if target:
        command.append("--target=" + target)
    command.append(str(path))
    return subprocess.run(command, capture_output=True, text=True, check=False, timeout=15)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    args = ap.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="oreslang-aot-attributes-") as directory:
        for target in ("host", "wasm32"):
            good = invoke(args.compiler, directory, "control_" + target,
                          "// @derive(Eq) in comments is not active metadata\n"
                          + "/* macro_rules! fake { () => { 42 }; } */\n" + BASE,
                          target)
            if good.returncode or "define" not in good.stdout:
                failures.append("positive control failed for " + target +
                                ": " + good.stderr[:350])
            for label, source in NEGATIVE_CASES.items():
                result = invoke(args.compiler, directory, label + "_" + target, source, target)
                if result.returncode == 0:
                    failures.append(label + "/" + target + " accepted unsupported syntax")
                if result.stdout.strip():
                    failures.append(label + "/" + target + " emitted IR on rejected source")
                if not result.stderr.strip():
                    failures.append(label + "/" + target + " gave no rejection diagnostic")
    if failures:
        for failure in failures:
            print("FAIL:", failure, file=sys.stderr)
        return 1
    print("PASS: native LLVM rejects all unsupported annotations/macros without IR (host + wasm32)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
