#!/usr/bin/env python3
"""Exact-head native ores.type-constraint.v1 descriptor and fail-closed gate."""

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


def load_contract(path):
    values = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        require(sep == "=" and key and key not in values,
                "malformed/duplicate contract key: " + line)
        values[key] = value
    return values


def compile_source(compiler, path, source, *extra):
    path.write_text(source, encoding="utf-8")
    return run([str(compiler), *extra, str(path)])


def descriptor_rows(output):
    rows = []
    for raw in output.decode("utf-8").splitlines():
        fields = raw.split("\t", 6)
        require(len(fields) == 7 and fields[0] == "descriptor",
                "malformed descriptor row: " + raw)
        rows.append({
            "schema": fields[1],
            "name": fields[2],
            "repr": fields[3],
            "validation_required": fields[4],
            "fingerprint": fields[5],
            "canonical": fields[6],
        })
    return rows


def require_descriptor(compiler, root, label, source, expected_name,
                       contract=None, contract_key=None):
    result = compile_source(
        compiler, root / (label + ".ores"), source, "--constraint-descriptors")
    require(result.returncode == 0,
            label + " descriptor rejection: " + repr(result.stderr[:1200]))
    require(result.stderr == b"", label + " descriptor mode emitted stderr")
    rows = descriptor_rows(result.stdout)
    require(len(rows) == 1, label + " expected exactly one descriptor")
    row = rows[0]
    require(row["schema"] == "ores.type-constraint.v1", label + " schema drift")
    require(row["name"] == expected_name, label + " qualified identity drift")
    require(row["repr"] == "i64", label + " representation drift")
    require(row["validation_required"] == "true",
            label + " validation-required marker drift")
    observed = hashlib.sha256(row["canonical"].encode("utf-8")).hexdigest()
    require(observed == row["fingerprint"], label + " fingerprint is not SHA-256 canonical form")
    if contract_key is not None:
        require(contract is not None, "golden contract missing")
        require(contract.get("constraint.validation_required") == "true",
                "golden validation-required marker drift")
        require(row["canonical"] == contract["constraint." + contract_key + ".canonical"],
                label + " canonical form differs from Java/LLVM golden vector")
        require(row["fingerprint"] == contract["constraint." + contract_key + ".fingerprint"],
                label + " fingerprint differs from Java/LLVM golden vector")
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    require(compiler.is_file(), "native compiler unavailable")

    contract = load_contract(
        Path(__file__).resolve().parents[1]
        / "contracts" / "v1" / "type-alias-extension-constraint-v1.properties"
    )

    with tempfile.TemporaryDirectory(prefix="ores-native-constraint-") as td:
        root = Path(td)

        root_declared = require_descriptor(
            compiler, root, "root_declared",
            "declare type Code = int with max 5 and min 0;\n",
            "Code", contract, "root.Code")
        root_defined = require_descriptor(
            compiler, root, "root_defined",
            "define type Code = int with min 0 and max 5;\n",
            "Code", contract, "root.Code")
        require(root_declared == root_defined,
                "declare/define constrained aliases must be descriptor-identical")

        i64_declared = require_descriptor(
            compiler, root, "root_i64_declared",
            "declare type Code = i64 with min 0 and max 5;\n",
            "Code", contract, "root.Code")
        require(i64_declared == root_declared,
                "int/i64 constrained aliases must be descriptor-identical")

        require_descriptor(
            compiler, root, "module_qualified",
            """define module api as
  declare type Code = int with min -2 and max 5;
end
""",
            "api.Code", contract, "api.Code")
        require_descriptor(
            compiler, root, "namespace_qualified",
            """namespace acme;
define module api as
  declare type Code = int with max 5 and min 0;
end
""",
            "acme.api.Code", contract, "acme.api.Code")

        mixed = compile_source(
            compiler,
            root / "mixed_scope.ores",
            """declare type RootCode = int with min 0 and max 5;
define module api as
  declare type ModuleCode = int with min 0 and max 5;
end
""",
            "--constraint-descriptors",
        )
        require(mixed.returncode == 0 and mixed.stderr == b"",
                "mixed root/module descriptor source rejected: " + repr(mixed.stderr[:1200]))
        mixed_rows = descriptor_rows(mixed.stdout)
        require([row["name"] for row in mixed_rows] == ["RootCode", "api.ModuleCode"],
                "descriptor ownership drifted across root/module declaration sites")

        min_only = require_descriptor(
            compiler, root, "min_only",
            "declare type Floor = int with min -9223372036854775808;\n",
            "Floor")
        require("min_inclusive_i64=-9223372036854775808" in min_only["canonical"],
                "signed i64 minimum lost")
        require("max_inclusive_i64" not in min_only["canonical"],
                "min-only descriptor fabricated max predicate")

        max_only = require_descriptor(
            compiler, root, "max_only",
            "declare type Ceiling = int with max 9223372036854775807;\n",
            "Ceiling")
        require("max_inclusive_i64=9223372036854775807" in max_only["canonical"],
                "signed i64 maximum lost")
        require("min_inclusive_i64" not in max_only["canonical"],
                "max-only descriptor fabricated min predicate")

        negatives = [
            (
                "duplicate_min",
                "declare type Code = int with min 0 and min 1;\n",
                "CONSTRAINT_DUPLICATE_MIN",
            ),
            (
                "duplicate_max",
                "declare type Code = int with max 5 and max 6;\n",
                "CONSTRAINT_DUPLICATE_MAX",
            ),
            (
                "empty_range",
                "declare type Code = int with min 6 and max 5;\n",
                "CONSTRAINT_EMPTY_RANGE",
            ),
            (
                "positive_overflow",
                "declare type Code = int with min 9223372036854775808;\n",
                "CONSTRAINT_I64_OVERFLOW",
            ),
            (
                "negative_overflow",
                "declare type Code = int with min -9223372036854775809;\n",
                "CONSTRAINT_I64_OVERFLOW",
            ),
            (
                "bool_representation",
                "declare type Flag = bool with min 0;\n",
                "CONSTRAINT_REPRESENTATION_UNSUPPORTED",
            ),
            (
                "generic_alias",
                "declare type Box<T> = int with min 0;\n",
                "TYPE_ONLY_GENERIC_UNSUPPORTED",
            ),
            (
                "trailing_and",
                "declare type Code = int with min 0 and;\n",
                "CONSTRAINT_PREDICATE_REQUIRED",
            ),
            (
                "runtime_parameter",
                """declare type Code = int with min 0 and max 5;
fnc read(Code code): int { return code; }
""",
                "CONSTRAINT_VALIDATION_BRIDGE_REQUIRED",
            ),
            (
                "alias_chain_laundering",
                """declare type Code = int with min 0 and max 5;
declare type Wrapped = Code;
fnc read(Wrapped code): int { return code; }
""",
                "CONSTRAINT_VALIDATION_BRIDGE_REQUIRED",
            ),
            (
                "extension_bypass",
                """declare type Code = int with min 0 and max 5;
define mtd double on Code(): int { return self + self; }
""",
                "CONSTRAINT_VALIDATION_BRIDGE_REQUIRED",
            ),
            (
                "trait_parameter_erasure",
                """declare type Code = int with min 0 and max 5;
declare trait Api as
  fnc read(Code value): int;
end
""",
                "CONSTRAINT_TYPE_ONLY_SIGNATURE_BRIDGE_REQUIRED",
            ),
            (
                "interface_return_erasure",
                """declare type Code = int with min 0 and max 5;
declare interface Api as
  fnc read(): Code;
end
""",
                "CONSTRAINT_TYPE_ONLY_SIGNATURE_BRIDGE_REQUIRED",
            ),
        ]
        negative_path = root / "negative.ores"
        for label, source, diagnostic in negatives:
            rejected = compile_source(compiler, negative_path, source)
            require(rejected.returncode != 0, label + " unexpectedly compiled")
            require(rejected.stdout == b"", label + " emitted LLVM IR on rejection")
            require(diagnostic.encode("utf-8") in rejected.stderr,
                    label + " wrong diagnostic: " + repr(rejected.stderr[:1200]))

    print(
        "PASS native constraint descriptor parity: 8 positive metadata cases, "
        "3 shared golden vectors, and 13 fail-closed negatives"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print("native constraint descriptor verification DENIED: " + str(exc), file=sys.stderr)
        sys.exit(1)
