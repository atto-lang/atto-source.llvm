#!/usr/bin/env python3
"""Execute actual .atto actor ownership guests against the native LLVM compiler.

This is compiler-backed ACTOR isolation evidence, not a hostile-tenant sandbox.
Keep exact-source, native-binary, and negative diagnostic evidence distinct.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

if not __debug__:
    raise SystemExit("security guest tests require assertions to remain enabled")

RUNTIME_FILES = (
    "ActorOwnedI64Heap.c",
    "NativeActorScheduler.c",
    "NativeOresVM.c",
    "NativeActorFuture.c",
    "SourceActorBootstrap.c",
    "NativeExecutionContextStore.c",
    "NativeContinuationStore.c",
    "NativeAwaitTraceBridge.c",
    "NativeLogicalTrace.c",
)

POSITIVE = (
    ("atto-actor-two-owned-heaps.atto", 2, [4, 4]),
    ("atto-actor-move-send.atto", 1, [2]),
)
NEGATIVE = (
    ("atto-actor-after-take-reject.atto", "use of explicitly moved int"),
    ("atto-actor-borrow-send-reject.atto", "borrowed reference may only be passed"),
)
FIELDS = {
    "constructor_on_worker", "released", "ready", "accepted",
    "processed", "abandoned", "last",
}

def execute(argv, timeout=90):
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout)

def digest(data):
    return hashlib.sha256(data).hexdigest()

def actor_receipt(line):
    parts = line.split()
    assert parts[0] == "SOURCE_ACTOR_SPAWN_STOP_VERIFIED", line
    items = {}
    for part in parts[1:]:
        key, sep, number = part.partition("=")
        assert sep and key in FIELDS and key not in items, part
        assert re.fullmatch(r"-?[0-9]+", number), part
        items[key] = int(number)
    assert set(items) == FIELDS, items
    assert items["released"] == 1, items
    assert items["constructor_on_worker"] in (0, 1), items
    assert items["ready"] in (0, 1), items
    if items["constructor_on_worker"] == 0:
        assert items["ready"] == items["processed"] == items["last"] == 0, items
    return items

def last_for(name, actor_index, processed):
    if processed == 0:
        return 0
    if name == "atto-actor-two-owned-heaps.atto":
        value = 0
        start = 1 if actor_index == 0 else 5
        for n in range(start, start + processed):
            value = 2 * (value + n)
        return value
    return sum((1, 2)[:processed])

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--auditor", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve(strict=True)
    fixtures = root / "tests" / "fixtures"
    runtime = root / "src" / "main" / "cpp" / "oreslang" / "runtime"
    compiler = Path(args.compiler).resolve(strict=True)
    assert compiler.name == "oreslang-llvmc", "not the source-owned compiler"
    assert os.access(compiler, os.X_OK), "compiler is not executable"
    assert all(path.is_file() and not path.is_symlink()
               for path in (fixtures / name for name, *_ in POSITIVE + NEGATIVE))
    before = {
        name: digest((fixtures / name).read_bytes())
        for name, *_ in POSITIVE + NEGATIVE
    }
    profile = "asan_ubsan" if args.sanitize else "native"
    with tempfile.TemporaryDirectory(prefix="atto-actor-isolation-") as directory:
        workspace = Path(directory)
        for name, expected in NEGATIVE:
            source = fixtures / name
            result = execute([str(compiler), str(source)])
            assert result.returncode != 0, (
                name, result.returncode, result.stderr)
            assert expected in result.stderr, (name, expected, result.stderr)
            assert not result.stdout.strip(), (name, "compiler emitted IR on rejection")
            assert digest(source.read_bytes()) == before[name], name
            print(f"ATTO_ACTOR_REJECT|case={name}|source_sha256={before[name]}|"
                  "guest_executed=false|ir_emitted=false", flush=True)

        for name, actor_count, expected_admitted in POSITIVE:
            source = fixtures / name
            flags = ["--experimental-owned-int"] if name == POSITIVE[0][0] else []
            generated = execute([str(compiler), *flags, str(source)])
            assert generated.returncode == 0 and generated.stdout.startswith("; ModuleID"), (
                name, generated.stderr, generated.returncode)
            ir = generated.stdout
            for symbol in ("oreslang_source_actor_spawn", "oreslang_source_actor_send",
                           "oreslang_source_actor_stop"):
                assert "call " in ir and symbol in ir, (name, symbol)
            if name == POSITIVE[0][0]:
                assert ir.count("call void @ores_actor_i64_heap_scoped_new(") == 2, name
                assert ir.count("call void @ores_actor_i64_heap_scoped_drop(") == 2, name

            ir_path = workspace / (name + ".ll")
            ir_path.write_text(ir, encoding="utf-8")
            audit = execute([args.auditor, str(ir_path)])
            assert audit.returncode == 0, (name, "no-GC IR auditor rejected source guest", audit.stderr)
            binary = workspace / (name + ".guest")
            opts = (["-O1", "-g", "-fno-omit-frame-pointer",
                     "-fsanitize=address,undefined"] if args.sanitize else ["-O1"])
            linker = [
                args.clang, *opts, "-x", "ir", str(ir_path),
                "-x", "c", *(str(runtime / p) for p in RUNTIME_FILES),
                "-std=c11", "-pthread", "-I", str(root / "src/main/cpp"),
                "-DORESLANG_SOURCE_ACTOR_TESTING", "-o", str(binary),
            ]
            linked = execute(linker)
            assert linked.returncode == 0, (name, linked.stderr)
            ran = execute([str(binary)])
            assert ran.returncode == 43, (name, ran.returncode, ran.stderr)
            routing = [
                line for line in ran.stderr.splitlines()
                if line.startswith("SOURCE_ACTOR_VM_ROUTING_VERIFIED")
            ]
            assert len(routing) == 1, (name, routing)
            assert "domains=4 main=1 cold_carriers=0" in routing[0], routing
            lines = [
                line for line in ran.stderr.splitlines()
                if line.startswith("SOURCE_ACTOR_SPAWN_STOP_VERIFIED")
            ]
            assert len(lines) == actor_count, (name, lines)
            for index, line in enumerate(lines):
                rec = actor_receipt(line)
                assert rec["accepted"] == expected_admitted[index], (name, index, rec)
                assert 0 <= rec["processed"] <= rec["accepted"], (name, rec)
                assert rec["abandoned"] == rec["accepted"] - rec["processed"], (name, rec)
                assert rec["last"] == last_for(name, index, rec["processed"]), (name, rec)
            assert digest(source.read_bytes()) == before[name], name
            print(f"ATTO_NATIVE_ACTOR_GUEST_PASS|case={name}|actors={actor_count}|"
                  f"profile={profile}|source_sha256={before[name]}|"
                  f"ir_sha256={digest(ir.encode('utf-8'))}|"
                  f"binary_sha256={digest(binary.read_bytes())}|exit=43", flush=True)
    print("ATTO_ACTOR_ISOLATION_MATRIX_PASS|positive=2|negative=2|"
          f"profile={profile}|compiler_sha256={digest(compiler.read_bytes())}|"
          "tenant_sandbox=NOT_VERIFIED", flush=True)

if __name__ == "__main__":
    main()
