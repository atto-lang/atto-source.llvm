#!/usr/bin/env python3
"""Exact native LLVM AOT counter: parser -> IR -> Clang -> actor-owned C ABI.

A compiler-generated on_message runs under real actor heap turn tokens.
The *separate* declared-actor + attached-state syntax remains fail-closed.
"""
import argparse
import pathlib
import hashlib
import subprocess
import tempfile

if not __debug__:
    raise SystemExit("native counter execution forbids Python -O")

def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=90)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--sanitize", action="store_true")
    opts = parser.parse_args()
    root = pathlib.Path(opts.root)
    fixture = root / "tests" / "fixtures" / "source-actor-counter-four.ores"
    unsupported = root / "tests" / "fixtures" / "declare-state-counter-native-unsupported.atto"
    harness = root / "tests" / "source_actor_counter_four_harness.c"
    shared_root = root / "contracts" / "declared-actor-stateful-counter-v2"
    identical_shared_blobs = {
        ".atto.pkg.toml": "500be9437a53a6767002280d60507587c947b92c",
        "src/counter/contract.atto": "213541736c415d30fc54bc0f413d8b0f77b623e4",
        "src/counter/actor.atto": "5135f72fac5b9ccb5c2a418f0d9f79bdd3f5ea32",
        "src/counter/state.atto": "5a63c3a307ec47e19025848fb979ceb42e9980c5",
        "src/counter/method.atto": "e7efbde0987013bc231eef40ec0524eeadc636e8",
        "src/counter/receive.atto": "e3f4b13503a1475b716756f54b3a77f28c8ccbc7",
        "src/counter/main.atto": "90d33cf97bfe553021cf340af8f62e4036a884e1",
    }
    for relative, expected in identical_shared_blobs.items():
        body = (shared_root / relative).read_bytes()
        digest = hashlib.sha1(
            b"blob " + str(len(body)).encode("ascii") + b"\0" + body
        ).hexdigest()
        assert digest == expected, (
            "java/llvm shared Counter V2 source identity drift", relative, digest)
    # The new sealed-state syntax has not been lowered by the LLVM backend.
    # In particular, never accept it through the older mutable field ABI.
    for path in ("src/counter/actor.atto", "src/counter/state.atto",
                 "src/counter/receive.atto"):
        attempt = run([opts.compiler, str(shared_root / path)])
        assert attempt.returncode != 0, (
            "unverified declared actor compiled on LLVM", path)
        assert not attempt.stdout.strip(), (
            "unverified critical state source emitted LLVM IR", path)

    native = root / "src" / "main" / "cpp" / "oreslang" / "runtime"
    include = root / "src" / "main" / "cpp"

    # An unsupported declared/attached actor must never emit runnable IR.
    rejected = run([opts.compiler, str(unsupported)])
    assert rejected.returncode != 0, "unverified declared actor source compiled"
    assert not rejected.stdout.strip(), "unverified declared actor emitted LLVM IR"

    compiled = run([opts.compiler, str(fixture)])
    assert compiled.returncode == 0, (compiled.returncode, compiled.stderr)
    assert compiled.stdout.startswith("; ModuleID"), "not LLVM IR"
    for symbol in ("oreslang_actor_Counter_init",
                   "oreslang_actor_Counter_on_message",
                   "ores_actor_i64_heap_field_write"):
        assert symbol in compiled.stdout, ("missing executable actor field codegen", symbol)

    with tempfile.TemporaryDirectory(prefix="atto-native-counter-four-") as folder:
        work = pathlib.Path(folder)
        ll = work / "counter.ll"
        exe = work / "counter"
        ll.write_text(compiled.stdout)
        flags = (["-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined"]
                 if opts.sanitize else ["-O1"])
        cmd = [opts.clang, *flags,
               "-x", "ir", str(ll),
               "-x", "c", str(harness), str(native / "ActorOwnedI64Heap.c"),
               "-std=c11", "-pthread", "-I", str(include), "-o", str(exe)]
        linked = run(cmd)
        assert linked.returncode == 0, (cmd, linked.stderr)
        executed = run([str(exe)])
        assert executed.returncode == 0, (executed.returncode, executed.stderr)
        assert executed.stderr.count("LLVM_COUNTER_FOUR_VERIFIED left=10 right=15 turns=8") == 1, (
            "native compiled Counter did not execute eight correct checked turns",
            executed.stderr)
        # The scheduler path processes a real compiled guest main with
        # independently owned actors. STOP is permitted to abandon already
        # accepted queued DATA, so validate each completed FIFO prefix.
        mailbox = root / "tests" / "fixtures" / "source-actor-counter-four-mailbox.ores"
        mailbox_ir = run([opts.compiler, str(mailbox)])
        assert mailbox_ir.returncode == 0, mailbox_ir.stderr
        assert "oreslang_source_actor_spawn" in mailbox_ir.stdout
        assert "oreslang_source_actor_send" in mailbox_ir.stdout
        assert "oreslang_source_actor_stop" in mailbox_ir.stdout
        linked_ll = work / "counter-mailbox.ll"
        linked_ll.write_text(mailbox_ir.stdout)
        scheduler_exe = work / "counter-mailbox"
        scheduler_sources = [
            "ActorOwnedI64Heap.c", "NativeActorScheduler.c", "NativeOresVM.c",
            "NativeActorFuture.c", "SourceActorBootstrap.c",
            "NativeExecutionContextStore.c", "NativeContinuationStore.c",
            "NativeAwaitTraceBridge.c", "NativeLogicalTrace.c",
        ]
        command = [opts.clang, *flags,
                   "-x", "ir", str(linked_ll), "-x", "c",
                   *(str(native / s) for s in scheduler_sources),
                   "-std=c11", "-pthread", "-I", str(include),
                   "-DORESLANG_SOURCE_ACTOR_TESTING", "-o", str(scheduler_exe)]
        compiled_scheduler = run(command)
        assert compiled_scheduler.returncode == 0, compiled_scheduler.stderr
        active = run([str(scheduler_exe)])
        assert active.returncode == 43, (active.returncode, active.stderr)
        receipts = [x.split() for x in active.stderr.splitlines()
                    if x.startswith("SOURCE_ACTOR_SPAWN_STOP_VERIFIED ")]
        assert len(receipts) == 2, (len(receipts), active.stderr)
        observed_totals = []
        for i, tokens in enumerate(receipts):
            parsed = {}
            for token in tokens[1:]:
                key, sep, value = token.partition("=")
                assert sep and key not in parsed and value.lstrip("-").isdigit(), token
                parsed[key] = int(value)
            assert set(parsed) == {
                "constructor_on_worker", "released", "ready",
                "accepted", "processed", "abandoned", "last"}, parsed
            assert parsed["released"] == 1, parsed
            messages = ([1, 2, 3, 4], [7, 8])[i]
            assert parsed["accepted"] == len(messages), parsed
            assert 0 <= parsed["processed"] <= len(messages), parsed
            assert parsed["abandoned"] == len(messages) - parsed["processed"], parsed
            assert parsed["last"] == sum(messages[:parsed["processed"]]), parsed
            observed_totals.append(parsed["last"])
        print("PASS LLVM scheduler actors: two independent FIFO prefixes",
              observed_totals)
    print("PASS real LLVM guest Counter: left=10 right=15, independent actor heaps, 8 turns")

if __name__ == "__main__":
    main()
