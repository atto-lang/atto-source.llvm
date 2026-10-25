#!/usr/bin/env python3
"""Executable IR contract for sealed actor state lowering."""
import argparse
import pathlib
import subprocess
import sys


class ContractFailure(Exception):
    pass


def require(condition, detail):
    if not condition:
        raise ContractFailure(detail)


def compile_fixture(compiler, fixture):
    result = subprocess.run(
        [compiler, str(fixture)],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    require(result.returncode == 0,
            f"{fixture.name}: compiler exited {result.returncode}: {result.stderr}")
    require(not result.stderr, f"{fixture.name}: unexpected stderr: {result.stderr}")
    require(result.stdout.startswith("; ModuleID"),
            f"{fixture.name}: missing LLVM module")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixtures", required=True)
    args = parser.parse_args()
    root = pathlib.Path(args.fixtures)

    empty_ir = compile_fixture(args.compiler, root / "sealed-state-empty-positive.ores")
    require("oreslang_actor_Idle_on_message" in empty_ir,
            "zero-schema actor missing compiler-generated message handler")
    require("call i64 @ores_actor_i64_heap_field_read" not in empty_ir,
            "zero-schema actor emitted a hidden state read")
    require("call void @ores_actor_i64_heap_field_write" not in empty_ir,
            "zero-schema actor emitted a hidden state write")

    read_ir = compile_fixture(args.compiler, root / "sealed-state-read-positive.ores")
    require("call i64 @ores_actor_i64_heap_field_read" in read_ir,
            "read callback emitted no actor state read")
    require("call void @ores_actor_i64_heap_field_write" not in read_ir,
            "read callback unexpectedly emitted actor state write")

    write_ir = compile_fixture(args.compiler, root / "sealed-state-write-positive.ores")
    require("call i64 @ores_actor_i64_heap_field_read" in write_ir,
            "read-modify-write callback emitted no state read")
    require("call void @ores_actor_i64_heap_field_write" in write_ir,
            "read-modify-write callback emitted no state write")

    write_only_ir = compile_fixture(
        args.compiler, root / "sealed-state-write-only-positive.ores")
    require("call void @ores_actor_i64_heap_field_write" in write_only_ir,
            "write-only callback emitted no state write")
    require("call i64 @ores_actor_i64_heap_field_read" not in write_only_ir,
            "write-only callback emitted a hidden state read")

    lexical_write_ir = compile_fixture(
        args.compiler, root / "sealed-state-using-write-positive.ores")
    require("call void @ores_actor_i64_heap_field_write" in lexical_write_ir,
            "lexical using state.write emitted no state write")
    require("call i64 @ores_actor_i64_heap_field_read" not in lexical_write_ir,
            "lexical write-only using acquired a hidden state read")

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ContractFailure as exc:
        print(f"sealed-state IR contract failed: {exc}", file=sys.stderr)
        sys.exit(1)
