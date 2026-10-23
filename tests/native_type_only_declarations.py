#!/usr/bin/env python3
"""Exact-source LLVM parser/linker admission tests for compiler-only nominal kinds.

The test intentionally requires native LLVM IR on success and no LLVM IR on
failure. It does not imply Java parity for generic traits, cross-unit imports,
type-alias use, general models, or a runtime representation.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def compile_source(compiler, source_text, target):
    target.write_text(source_text, encoding="utf-8")
    return subprocess.run(
        [compiler, str(target)], capture_output=True, text=True, timeout=40
    )


def main():
    args = argparse.ArgumentParser()
    args.add_argument("--compiler", required=True)
    opts = args.parse_args()
    main_fn = "\npub fnc main(): int { return 0; }\n"
    positive = [
        ("canonical private and public type aliases",
         "declare type LocalId = int;\ndeclare pub type PublicId = bool;\n" + main_fn),
        ("root trait and interface signatures",
         "declare trait Tagged as\n fnc tag(): int;\nend\n"
         "declare pub interface Endpoint as\n fnc request() => bool;\nend\n" + main_fn),
        ("module-contained public declarations",
         "define module api as\n"
         " declare pub type Id = int;\n"
         " declare pub trait Tagged as\n fnc tag(int value) => int;\nend\n"
         " declare pub interface Endpoint as\n fnc request() => int;\nend\n"
         " pub fnc main(): int { return 0; }\nend\n"),
        ("legacy define spellings",
         "define type Id = int;\ndefine trait Tagged as end\n"
         "define interface Endpoint as end\n" + main_fn),
        ("compatibility public visibility order",
         "pub declare type Id = int;\npub declare trait Tagged as end\n"
         "pub declare interface Endpoint as end\n" + main_fn),
        ("same-kind inheritance",
         "declare trait Parent as end\n"
         "declare pub trait Child extends Parent as end\n"
         "declare interface Api as end\n"
         "declare interface Extended extends Api as end\n" + main_fn),
        ("contextual declare function identifier",
         "fnc declare(): int { return 0; }\n" + main_fn),
        ("method arity-overload signatures",
         "declare interface Api as\n"
         " fnc get() => int;\n fnc get(int key) => int;\nend\n" + main_fn),
        ("named type-only method formals",
         "declare trait Tagged { fnc tag(value: int): bool; }\n" + main_fn),
    ]
    negative = [
        ("reject concrete declare model",
         "declare model M as Pass() end\n" + main_fn, "TYPE_ONLY_INVALID_KIND"),
        ("reject concrete declare actor",
         "declare actor A as end\n" + main_fn, "TYPE_ONLY_INVALID_KIND"),
        ("reject unsupported scalar alias target",
         "declare type Id = string;\n" + main_fn, "TYPE_ONLY_UNSUPPORTED_TYPE"),
        ("reject generic trait without native contract proof",
         "declare trait Generic<T> as end\n" + main_fn, None),
        ("reject trait extends interface",
         "declare interface Parent as end\ndeclare trait Child extends Parent as end\n"
         + main_fn, "TYPE_PARENT_KIND_MISMATCH"),
        ("reject interface extends trait",
         "declare trait Parent as end\ndeclare interface Child extends Parent as end\n"
         + main_fn, "TYPE_PARENT_KIND_MISMATCH"),
        ("reject unknown trait parent",
         "declare trait Child extends Missing as end\n" + main_fn, "TYPE_PARENT_UNKNOWN"),
        ("reject cycles",
         "declare trait A extends B as end\ndeclare trait B extends A as end\n"
         + main_fn, "TYPE_PARENT_CYCLE"),
        ("reject duplicate type declarations",
         "declare type Item = int;\ndeclare interface Item as end\n"
         + main_fn, "TYPE_DECL_NAME_COLLISION"),
        ("reject interface/class name collision",
         "declare interface Item as end\ndefine class Item as end\n"
         + main_fn, "TYPE_DECL_NAME_COLLISION"),
        ("reject duplicate method overload slot",
         "declare trait Api as\n fnc read() => int;\n fnc read(): bool;\nend\n"
         + main_fn, "TYPE_ONLY_DUPLICATE_MEMBER"),
        ("reject invalid signature arrow",
         "declare interface Api as\n fnc read() = > int;\nend\n"
         + main_fn, "TYPE_ONLY_BAD_ARROW"),
        ("reject duplicate visibility",
         "declare pub pub interface Bad as end\n" + main_fn, "TYPE_ONLY_INVALID_KIND"),
        ("reject runtime use of type-only trait",
         "declare trait Tagged as end\npub fnc main(): int { return Tagged; }\n",
         None),
        ("reject LLVM model execution until parity verified",
         "define model M as Pass() end\n" + main_fn, "MODEL_BACKEND_UNSUPPORTED"),
        ("reject effectful trait method body",
         "declare trait Api as fnc read(): int { return 1; } end\n" + main_fn,
         None),
    ]
    with tempfile.TemporaryDirectory(prefix="ores-native-typeonly-") as root:
        source = Path(root) / "typeonly.ores"
        for label, program in positive:
            result = compile_source(opts.compiler, program, source)
            if result.returncode or result.stderr or not result.stdout.startswith("; ModuleID"):
                raise AssertionError((label, result.returncode, result.stderr, result.stdout[:500]))
            # Type-only declarations cannot silently lower a named runtime value.
            for forbidden in ("@Tagged", "@Endpoint", "@LocalId", "@PublicId"):
                if forbidden in result.stdout:
                    raise AssertionError((label, "type-only runtime emission", forbidden))
        for label, program, diagnostic in negative:
            result = compile_source(opts.compiler, program, source)
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError((label, "invalid source admitted", result.stdout[:500],
                                      result.stderr[:500]))
            if diagnostic and diagnostic not in result.stderr:
                raise AssertionError((label, "wrong diagnostic", diagnostic, result.stderr))
    print(f"PASS {len(positive)} positive and {len(negative)} negative native type-only cases")


if __name__ == "__main__":
    main()
