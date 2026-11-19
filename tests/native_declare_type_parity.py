#!/usr/bin/env python3
"""Native LLVM *source compiler* acceptance/rejection; requires a built llvmc."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ENTRY = "pub fnc main(): int { return 7; }\n"

# Complete aliases supersede the removed declare-then-define protocol.
# Keep source admission, runtime ABI and descriptor-only types distinct.
POSITIVE = {
    "complete-alias-chain-fixture": (Path(__file__).with_name("fixtures") / "forward-scalar-alias-parity.ores").read_text(encoding="utf-8"),
    "declared-int": "declare type Code = int; fnc id(Code n):Code{return n;} pub fnc main():int{return id(7);}",
    "defined-int": "define type Code = int; fnc id(Code n):Code{return n;} pub fnc main():int{return id(7);}",
    "public-int": "declare pub type Code = int; fnc id(Code n):Code{return n;} pub fnc main():int{return id(7);}",
    "prefix-public-int": "pub declare type Code = int;" + ENTRY,
    "legacy-public-define": "pub define type Code = int;" + ENTRY,
    "module-scope-alias": "define module foo as declare type Code = int; fnc id(Code n):Code{return n;} pub fnc main():int{return id(7);} end",
    "bool-alias": "declare type Flag = bool; fnc ready(Flag n):Flag{return n;} pub fnc main():int{return ready(true)?7:0;}",
    "forward-target-chain": "declare type Code = Raw; declare type Raw = int; fnc id(Code n):Code{return n;} pub fnc main():int{return id(7);}",
    "defined-chain": "define type Raw = int; define type Code = Raw; fnc id(Code n):Code{return n;} pub fnc main():int{return id(7);}",
    "descriptor-only-trait": "define trait Printable as end " + ENTRY,
    "descriptor-only-interface": "define interface Named as end " + ENTRY,
}
NEGATIVE = {
    "missing-definition": ("declare type Missing;" + ENTRY, "TYPE_ONLY_INCOMPLETE_DEFINITION"),
    "incomplete-before-definition": ("declare type Code; define type Code = int;" + ENTRY, "TYPE_ONLY_INCOMPLETE_DEFINITION"),
    "duplicate-declaration": ("declare type Code = int; declare type Code = int;" + ENTRY, "TYPE_DECL_NAME_COLLISION"),
    "duplicate-definition": ("define type Code = int; define type Code = bool;" + ENTRY, "TYPE_DECL_NAME_COLLISION"),
    "visibility-widen": ("declare type Code = int; define pub type Code = int;" + ENTRY, "TYPE_DECL_NAME_COLLISION"),
    "visibility-narrow": ("declare pub type Code = int; define type Code = int;" + ENTRY, "TYPE_DECL_NAME_COLLISION"),
    "refined-runtime-value": ("define type Code = int with min 0 and max 5; fnc id(Code n):Code{return n;}" + ENTRY, "CONSTRAINT_VALIDATION_BRIDGE_REQUIRED"),
    "refined-chain-runtime-value": ("define type Code = int with min 0 and max 5; declare type Raw = Code; fnc id(Raw n):Raw{return n;}" + ENTRY, "CONSTRAINT_VALIDATION_BRIDGE_REQUIRED"),
    "unsupported-refinement-expression": ("define type Code = int with value gte 0 and value lte 5;" + ENTRY, "CONSTRAINT"),
    "unsupported-refinement-language": ("define type Code = int as positive integer;" + ENTRY, "expected ';'"),
    "string-length-refinement": ("define type Code = string with min_length 2 and max_length 4 and chars ascii_letters;" + ENTRY, "CONSTRAINT"),
    "string-ascii-refinement": ("define type Code = string with chars ascii_uppercase;" + ENTRY, "CONSTRAINT"),
    "string-unicode-refinement": ("define type Code = string with chars uppercase;" + ENTRY, "CONSTRAINT"),
    "string-han-refinement": ("define type Code = string with chars han;" + ENTRY, "CONSTRAINT"),
    "generic-declare": ("declare type Code<T> = int;" + ENTRY, "TYPE_ONLY_GENERIC_UNSUPPORTED"),
    "generic-define": ("define type Code<T> = int;" + ENTRY, "TYPE_ONLY_GENERIC_UNSUPPORTED"),
    "incomplete-trait": ("declare trait Printable;" + ENTRY, "expected 'as'"),
    "incomplete-interface": ("declare interface Named;" + ENTRY, "expected 'as'"),
    "enum-runtime-layout": ("define enum Status as Success = 1; end " + ENTRY, "native user-defined tagged enum"),
    "actor-not-type-only": ("declare actor Worker;" + ENTRY, "NATIVE_ACTOR_FORWARD_UNRESOLVED"),
    "enum-not-type-only": ("declare enum Status;" + ENTRY, "TYPE_ONLY_INVALID_KIND"),
    "non-scalar-generic-alias": ("define type Code = Option<int>;" + ENTRY, "expected ';'"),
    "self-alias": ("define type Code = Code;" + ENTRY, "TYPE_ALIAS_CYCLE"),
    "mutual-alias-cycle": ("define type One = Two; define type Two = One;" + ENTRY, "TYPE_ALIAS_CYCLE"),
    "unknown-alias-target": ("define type Raw = Missing;" + ENTRY, "TYPE_ALIAS_UNKNOWN_TARGET"),
    "builtin-shadowing": ("define type int = bool;" + ENTRY, "identifier"),
    "string-builtin-shadowing": ("define type String = int;" + ENTRY, "TYPE_DECL_RESERVED_BUILTIN"),
    "class-collision": ("define type Box = int; define class Box as pub let int count = 1; end " + ENTRY, "TYPE_DECL_NAME_COLLISION"),
    "actor-collision": ("define type Worker = int; define actor Worker as let count:int=0; pub fnc on_message(int n):int{return n;} end " + ENTRY, "TYPE_DECL_NAME_COLLISION"),
    "borrow-bool-alias": ("declare type Flag = bool; fnc invalid(borrow Flag n):bool{return n;}" + ENTRY, "native bool borrow ABI is not yet admitted"),
    "bool-as-int-return": ("declare type Flag = bool; fnc bad(Flag n):int{return n;}" + ENTRY, "native function return requires int, got bool"),
    "int-as-bool-argument": ("declare type Code = int; fnc id(Code n):Code{return n;} pub fnc main():int{return id(true);}", "requires int, got bool"),
    "trait-as-value": ("define trait Box as end fnc invalid(Box n):int{return 7;}" + ENTRY, "TYPE_ALIAS_KIND_MISMATCH"),
    "use-before-declaration": ("fnc id(Code n):Code{return n;} define type Code = int;" + ENTRY, "unknown native class type"),
}

def run(compiler, root, name, source):
    path = root / (name + ".ores")
    path.write_text(source)
    return subprocess.run([compiler, str(path)], capture_output=True,
                          text=True, timeout=30, check=False)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--clang", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-native-declare-parity-") as tmp:
        root = Path(tmp)
        for name, source in POSITIVE.items():
            p = run(args.compiler, root, name, source)
            if p.returncode or not p.stdout.startswith("; ModuleID"):
                raise AssertionError(("positive native source failed", name, p.returncode,
                                      p.stderr, p.stdout[:400]))
            ir_file = root / (name + ".ll")
            ir_file.write_text(p.stdout)
            executable = root / (name + ".exe")
            linked = subprocess.run(
                [args.clang, "-O1", "-x", "ir", str(ir_file), "-o", str(executable)],
                capture_output=True, text=True, timeout=60, check=False)
            if linked.returncode:
                raise AssertionError(("LLVM IR link failed", name, linked.stderr))
            ran = subprocess.run([str(executable)], capture_output=True, text=True,
                                 timeout=30, check=False)
            if ran.returncode != 7:
                raise AssertionError(("native guest exit mismatch", name, ran.returncode,
                                      ran.stdout, ran.stderr))
        for name, (source, expected) in NEGATIVE.items():
            p = run(args.compiler, root, name, source)
            if p.returncode == 0 or p.stdout.strip() or expected not in p.stderr:
                raise AssertionError(("unsupported source admitted or missing diagnostic",
                                      name, p.returncode, p.stderr, p.stdout[:400]))
    print("PASS native complete scalar aliases: "
          f"{len(POSITIVE)} compiled and executed, {len(NEGATIVE)} rejected; "
          "type/trait/interface full JVM parity NOT CLAIMED")

if __name__ == "__main__":
    main()
