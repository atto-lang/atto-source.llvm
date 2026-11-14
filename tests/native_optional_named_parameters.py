#!/usr/bin/env python3
"""Native AttoLang compiler evidence: independent T? and name? fail closed.

This test runs the built native LLVM compiler against actual .atto sources.
The positive controls must produce LLVM IR that Clang links and runs. All
unproved optional formals/defaults must return 1 without emitting LLVM IR.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(*command):
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=35, check=False)


def require(ok, label, result=None):
    if not ok:
        raise AssertionError(f"{label}: {result!r}")


def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("--compiler", required=True)
    cli.add_argument("--clang", required=True)
    args = cli.parse_args()

    positive = [
        ("positional_call", "fnc sum(int first,int second):int{return first+second;}"
                            "pub fnc main():int{return sum(40,2);}"),
        ("named_scalar_call", "fnc sum(int first,int second):int{return first+second;}"
                             "pub fnc main():int{return sum(second:2,first:40);}"),
    ]
    rejected = [
        ("required_option", "fnc f(int? value):int{return 42;}",
         "E_OPTION_TYPE_ABI_UNSUPPORTED"),
        ("nontrailing_option", "fnc f(int first,int? next):int{return first;}",
         "E_OPTION_TYPE_ABI_UNSUPPORTED"),
        ("option_bool", "fnc f(bool? value):int{return 42;}",
         "E_OPTION_TYPE_ABI_UNSUPPORTED"),
        ("name_omission", "fnc f(int value?):int{return 42;}",
         "E_OPTIONAL_FORMAL_ABI_UNSUPPORTED"),
        ("nontrailing_omission", "fnc f(int first?,int second):int{return second;}",
         "E_OPTIONAL_FORMAL_ABI_UNSUPPORTED"),
        ("default_omission", "fnc f(int value?=42):int{return value;}",
         "E_OPTIONAL_FORMAL_ABI_UNSUPPORTED"),
        ("double_question", "fnc f(int? value?):int{return 42;}",
         "E_OPTION_TYPE_ABI_UNSUPPORTED"),
        ("default_without_marker", "fnc f(int value=42):int{return value;}",
         "E_PARAMETER_DEFAULT_REQUIRES_NAME_QUESTION"),
        ("optional_return", "fnc f():int?{return None;}",
         "E_OPTION_RETURN_ABI_UNSUPPORTED"),
    ]

    with tempfile.TemporaryDirectory(prefix="atto-native-parameter-presence-") as tmp:
        root = Path(tmp)
        for label, source in positive:
            path = root / (label + ".atto")
            path.write_text(source, encoding="utf-8")
            emitted = run(args.compiler, str(path))
            require(emitted.returncode == 0 and
                    "define" in emitted.stdout and not emitted.stderr,
                    label + " did not compile to IR", emitted)
            ir = root / (label + ".ll")
            ir.write_text(emitted.stdout, encoding="utf-8")
            binary = root / label
            linked = run(args.clang, "-O1", str(ir), "-o", str(binary))
            require(linked.returncode == 0, label + " link", linked)
            executed = run(str(binary))
            require(executed.returncode == 42, label + " exit", executed)
        for label, source, diagnostic in rejected:
            path = root / (label + ".atto")
            path.write_text(source, encoding="utf-8")
            result = run(args.compiler, str(path))
            require(result.returncode == 1 and not result.stdout and
                    diagnostic in result.stderr,
                    label + " failed strict no-IR diagnostic", result)

    print("PASS: 2 native compiled-and-executed named/positional controls;"
          " 9 optional/type/default compiler rejections; not Option ABI parity")


if __name__ == "__main__":
    main()
