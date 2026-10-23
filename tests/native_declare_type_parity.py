#!/usr/bin/env python3
"""Native LLVM *source compiler* acceptance/rejection; requires a built llvmc."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ENTRY = "pub fnc main(): int { return 7; }\n"

POSITIVE = {
    "shared-java-llvm-executable-fixture": (
        Path(__file__).with_name("fixtures")
        / "forward-scalar-alias-parity.ores"
    ).read_text(encoding="utf-8"),
    "scalar-int-forward": (
        "declare type Code;\n"
        "define type Code = int;\n"
        "fnc id(Code code): Code { return code; }\n"
        "pub fnc main(): int { return id(7); }\n"
    ),
    "public-int-forward": (
        "declare pub type Code;\n"
        "define pub type Code = int;\n"
        "fnc id(Code code): Code { return code; }\n"
        "pub fnc main(): int { return id(7); }\n"
    ),
    "legacy-public-define": (
        "declare pub type Code;\n"
        "pub define type Code = int;\n" + ENTRY
    ),
    "module-scope-alias": (
        "define module foo as\n"
        "  declare type Code;\n"
        "  define type Code = int;\n"
        "  fnc id(Code code): Code { return code; }\n"
        "  pub fnc main(): int { return id(7); }\n"
        "end\n"
    ),
    "bool-alias": (
        "declare type Flag;\n"
        "define type Flag = bool;\n"
        "fnc ready(Flag value): Flag { return value; }\n"
        "pub fnc main(): int { return ready(true) ? 7 : 0; }\n"
    ),
    "forward-use-before-definition": (
        "declare type Code;\n"
        "fnc id(Code code): Code { return code; }\n"
        "define type Code = int;\n"
        "pub fnc main(): int { return id(7); }\n"
    ),
    "forward-bool-before-definition": (
        "declare type Flag;\n"
        "fnc ready(Flag value): Flag { return value; }\n"
        "define type Flag = bool;\n"
        "pub fnc main(): int { return ready(true) ? 7 : 0; }\n"
    ),
    "module-forward-before-definition": (
        "define module foo as\n"
        "  declare type Code;\n"
        "  fnc id(Code code): Code { return code; }\n"
        "  define type Code = int;\n"
        "  pub fnc main(): int { return id(7); }\n"
        "end\n"
    ),
    "forward-alias-dependency-chain": (
        "declare type Code;\n"
        "declare type Raw;\n"
        "fnc id(Code code): Code { return code; }\n"
        "define type Code = Raw;\n"
        "define type Raw = int;\n"
        "pub fnc main(): int { return id(7); }\n"
    ),
    "alias-chain": (
        "type Raw = int;\n"
        "declare type Code;\n"
        "define type Code = Raw;\n"
        "fnc id(Code code): Code { return code; }\n"
        "pub fnc main(): int { return id(7); }\n"
    ),
}

NEGATIVE = {
    "missing-definition": ("declare type Missing;\n"+ENTRY, "unresolved native forward type"),
    "duplicate-forward": (
        "declare type Code;\ndeclare type Code;\ndefine type Code = int;\n"+ENTRY,
        "duplicate native forward"
    ),
    "visibility-widen": (
        "declare type Code;\ndefine pub type Code = int;\n"+ENTRY,
        "visibility mismatch"
    ),
    "visibility-narrow": (
        "declare pub type Code;\ndefine type Code = int;\n"+ENTRY,
        "visibility mismatch"
    ),
    "duplicate-definition": (
        "type Code = int;\ntype Code = bool;\n"+ENTRY,
        "duplicate native type alias"
    ),
    "generic-forward": (
        "declare type Code<T>;\ndefine type Code = int;\n"+ENTRY,
        "generic type alias forward"
    ),
    "generic-definition": (
        "define type Code<T> = int;\n"+ENTRY,
        "generic type alias"
    ),
    "unsupported-trait": ("declare trait Printable;\n"+ENTRY, "native trait/interface forward"),
    "unsupported-trait-definition": (
        "define trait Printable as end\n"+ENTRY,
        "native trait/interface definitions"
    ),
    "unsupported-interface-definition": (
        "define interface Named as end\n"+ENTRY,
        "native trait/interface definitions"
    ),
    "unsupported-enum-definition": (
        "define enum Status as Success = 1; end\n"+ENTRY,
        "native user-defined tagged enum"
    ),
    "unsupported-interface": ("declare interface Named;\n"+ENTRY, "native trait/interface forward"),
    "unsupported-actor": ("declare actor Worker;\n"+ENTRY, "native declare supports only"),
    "unsupported-enum": ("declare enum Status;\n"+ENTRY, "native declare supports only"),
    "non-scalar-alias": (
        "declare type Code;\ndefine type Code = Option<int>;\n"+ENTRY,
        "scalar target"
    ),
    "self-alias": (
        "declare type Code;\ndefine type Code = Code;\n"+ENTRY,
        "native type alias cycle"
    ),
    "mutual-alias-cycle": (
        "declare type One;\n"
        "declare type Two;\n"
        "define type One = Two;\n"
        "define type Two = One;\n"+ENTRY,
        "native type alias cycle"
    ),
    "alias-forward-without-definition": (
        "declare type Code;\n"
        "define type Raw = Code;\n"+ENTRY,
        "unresolved native forward type"
    ),
    "builtin-shadowing": ("type int = bool;\n"+ENTRY, "reserved"),
    "class-collision": (
        "define type Box = int;\n"
        "define class Box as pub let int count = 1; end\n"+ENTRY,
        "collides with type alias"
    ),
    "actor-collision": (
        "define type Worker = int;\n"
        "define actor Worker as\n"
        "  let count: int = 0;\n"
        "  pub fnc on_message(int value): int { return value; }\n"
        "end\n"+ENTRY,
        "collides with type alias"
    ),
    "use-before-forward-not-admitted": (
        "fnc id(Code code): Code { return code; }\n"
        "declare type Code;\n"
        "define type Code = int;\n"+ENTRY,
        "native return type"
    ),
    "borrow-bool-through-forward": (
        "declare type Flag;\n"
        "fnc invalid(borrow Flag flag): bool { return flag; }\n"
        "define type Flag = bool;\n" + ENTRY,
        "native bool borrow ABI is not yet admitted"
    ),
    "bool-disguised-as-int-return": (
        "declare type Flag;\n"
        "fnc bad(Flag flag): int { return flag; }\n"
        "define type Flag = bool;\n" + ENTRY,
        "native function return requires int, got bool"
    ),
    "int-disguised-as-bool-argument": (
        "declare type Code;\n"
        "fnc id(Code code): Code { return code; }\n"
        "define type Code = int;\n"
        "pub fnc main(): int { return id(true); }\n",
        "argument 'code' requires int, got bool"
    ),
    "forward-class-impersonation": (
        "declare type Box;\n"
        "define class Box as pub let int value = 1; end\n" + ENTRY,
        "unresolved native forward type"
    ),
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
    print("PASS native source-forward scalar type: "
          f"{len(POSITIVE)} compiled and executed, {len(NEGATIVE)} rejected; "
          "type/trait/interface full JVM parity NOT CLAIMED")

if __name__ == "__main__":
    main()
