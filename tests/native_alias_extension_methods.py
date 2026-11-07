#!/usr/bin/env python3
"""Exact-head native alias extension-method compiler/LLVM/Clang execution gate."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(argv):
    return subprocess.run(
        argv,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=30,
        check=False,
    )


def load_golden_contract(path):
    values = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        require("=" in line, "malformed golden contract line: " + line)
        key, value = line.split("=", 1)
        require(key and key not in values, "duplicate golden contract key: " + key)
        values[key] = value
    return values


def verify_golden_contract(values):
    require(values.get("schema") == "ores.alias-extension-constraint.v1",
            "unexpected alias/extension golden schema")
    require(values.get("extension.reserved_names") == "init,main",
            "reserved extension-name parity drift")
    require(values.get("extension.overload_identity") == "name+explicit-arity",
            "extension overload identity parity drift")
    require(values.get("constraint.schema") == "ores.type-constraint.v1",
            "constraint schema parity drift")
    require(values.get("constraint.representation") == "i64",
            "constraint representation parity drift")
    for identity in ("root.Code", "api.Code", "acme.api.Code"):
        canonical = values.get("constraint." + identity + ".canonical")
        expected = values.get("constraint." + identity + ".fingerprint")
        require(canonical is not None and expected is not None,
                "missing constraint golden vector for " + identity)
        observed = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
        require(observed == expected,
                "constraint golden fingerprint mismatch for " + identity)


def compile_source(compiler, source_path, source_text):
    source_path.write_text(source_text, encoding="utf-8")
    return run([str(compiler), str(source_path)])


def compile_and_run(compiler, clang, root, label, source_text, expected_exit, marker):
    source = root / (label + ".ores")
    ir = root / (label + ".ll")
    exe = root / label
    compiled = compile_source(compiler, source, source_text)
    require(
        compiled.returncode == 0 and compiled.stdout.startswith(b"; ModuleID"),
        label + " compiler rejection: " + repr(compiled.stderr[:1200]),
    )
    require(compiled.stderr == b"", label + " compiler emitted stderr")
    require(marker.encode("utf-8") in compiled.stdout,
            label + " missing compiler-owned extension slot " + marker)
    require(b"@Code" not in compiled.stdout and b"@Other" not in compiled.stdout,
            label + " emitted runtime nominal alias object")
    ir.write_bytes(compiled.stdout)
    linked = run([str(clang), "-O0", "-x", "ir", str(ir), "-o", str(exe)])
    require(linked.returncode == 0 and exe.is_file(),
            label + " clang link failure: " + repr(linked.stderr[:1200]))
    observed = run([str(exe)])
    require(observed.returncode == expected_exit,
            label + " exit mismatch: " + repr(observed.returncode))
    require(observed.stdout == b"" and observed.stderr == b"",
            label + " executable produced unexpected output")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    clang = args.clang.resolve()
    require(compiler.is_file(), "native compiler unavailable")
    require(clang.is_file(), "clang unavailable")

    contract = load_golden_contract(
        Path(__file__).resolve().parents[1]
        / "contracts" / "v1" / "type-alias-extension-constraint-v1.properties"
    )
    verify_golden_contract(contract)
    receiver_diag = contract["extension.diagnostic.receiver"]
    arity_diag = contract["extension.diagnostic.arity"]
    reserved_diag = contract["extension.diagnostic.reserved"]
    self_diag = contract["extension.diagnostic.self_shadow"]
    duplicate_diag = contract["extension.diagnostic.duplicate"]

    positives = [
        (
            "declare_zero_arity",
            """declare type Code = int;
define mtd double on Code(): int { return self + self; }
pub fnc main(): int {
  val code: Code = 21;
  return code.double();
}
""",
            42,
            "$mtd$Code$double",
        ),
        (
            "define_named_argument",
            """define type Code = int;
define pub mtd plus on Code(int delta): Code { return self + delta; }
pub fnc main(): int {
  val code: Code = 40;
  return code.plus(delta: 2);
}
""",
            42,
            "$mtd$Code$plus",
        ),
        (
            "forward_method_declaration",
            """declare type Code = int;
fnc apply(Code code): int { return code.double(); }
define mtd double on Code(): int { return self + self; }
pub fnc main(): int {
  val code: Code = 21;
  return apply(code);
}
""",
            42,
            "$mtd$Code$double",
        ),
        (
            "bool_alias_identity",
            """declare type Flag = bool;
define mtd invert on Flag(): bool { return !self; }
pub fnc main(): int {
  val ready: Flag = false;
  return ready.invert() ? 1 : 0;
}
""",
            1,
            "$mtd$Flag$invert",
        ),
        (
            "arity_overload",
            """declare type Code = int;
define mtd choose on Code(): int { return self; }
define mtd choose on Code(int delta): int { return self + delta; }
pub fnc main(): int {
  val code: Code = 21;
  return code.choose() + code.choose(0);
}
""",
            42,
            "$mtd$Code$choose$arity1",
        ),
    ]

    negatives = [
        (
            "raw_int_cannot_inherit",
            """declare type Code = int;
define mtd double on Code(): int { return self + self; }
pub fnc main(): int {
  val code: int = 21;
  return code.double();
}
""",
            receiver_diag,
        ),
        (
            "other_alias_cannot_inherit",
            """declare type Code = int;
declare type Other = int;
define mtd double on Code(): int { return self + self; }
pub fnc main(): int {
  val other: Other = 21;
  return other.double();
}
""",
            receiver_diag,
        ),
        (
            "primitive_receiver_declaration_rejected",
            """define mtd double on int(): int { return self + self; }
pub fnc main(): int { return 0; }
""",
            "EXTENSION_RECEIVER_REQUIRES_NAMED_ALIAS",
        ),
        (
            "unknown_receiver_declaration_rejected",
            """define mtd double on Missing(): int { return self + self; }
pub fnc main(): int { return 0; }
""",
            "EXTENSION_RECEIVER_UNKNOWN",
        ),
        (
            "duplicate_slot_rejected",
            """declare type Code = int;
define mtd plus on Code(int x): int { return self + x; }
define mtd plus on Code(int y): int { return self + y; }
pub fnc main(): int { return 0; }
""",
            duplicate_diag,
        ),
        (
            "correct_alias_wrong_arity_rejected",
            """declare type Code = int;
define mtd plus on Code(int x): int { return self + x; }
pub fnc main(): int {
  val code: Code = 40;
  return code.plus();
}
""",
            arity_diag,
        ),
        (
            "reserved_main_rejected",
            """declare type Code = int;
define mtd main on Code(): int { return self; }
pub fnc entry(): int { return 0; }
""",
            reserved_diag,
        ),
        (
            "reserved_init_rejected",
            """declare type Code = int;
define mtd init on Code(): int { return self; }
pub fnc main(): int { return 0; }
""",
            reserved_diag,
        ),
        (
            "explicit_self_shadow_rejected",
            """declare type Code = int;
define mtd bad on Code(int self): int { return self; }
pub fnc main(): int { return 0; }
""",
            self_diag,
        ),
    ]

    with tempfile.TemporaryDirectory(prefix="ores-native-extension-") as td:
        root = Path(td)
        for label, source, expected, marker in positives:
            compile_and_run(compiler, clang, root, label, source, expected, marker)

        # An alias extension sharing a name/arity with an ordinary Array member
        # must not capture or reject the non-scalar Array receiver.
        array_collision = compile_source(
            compiler,
            root / "array_member_collision.ores",
            """declare type Code = int;
define mtd add on Code(int delta): int { return self + delta; }
pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(42);
  return values.get(0);
}
""",
        )
        require(array_collision.returncode == 0,
                "array member collided with alias extension: "
                + repr(array_collision.stderr[:1200]))
        require(array_collision.stderr == b"",
                "array member collision compiler emitted stderr")
        require(b"@oreslang_int_array_try_add" in array_collision.stdout,
                "array add lowering was captured by alias extension dispatch")

        source_path = root / "negative.ores"
        for label, source, diagnostic in negatives:
            rejected = compile_source(compiler, source_path, source)
            require(rejected.returncode != 0, label + " unexpectedly compiled")
            require(rejected.stdout == b"", label + " emitted LLVM IR on rejection")
            require(diagnostic.encode("utf-8") in rejected.stderr,
                    label + " wrong diagnostic: " + repr(rejected.stderr[:1200]))

    print(
        "PASS 5 executable alias-extension cases, 1 array-collision case, "
        "and 9 fail-closed receiver/slot/name/arity/self cases"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print("native alias extension verification DENIED: " + str(exc), file=sys.stderr)
        sys.exit(1)
