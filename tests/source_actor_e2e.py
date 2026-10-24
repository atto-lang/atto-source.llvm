#!/usr/bin/env python3
"""Real Oreslang source -> LLVM -> Clang -> native actor scheduler regression."""
import argparse
import pathlib
import re
import subprocess
import tempfile

# Every safety assertion below is part of the CI gate: never silently remove it.
if not __debug__:
    raise SystemExit("source actor execution gate forbids optimized Python (-O)")

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=90, **kw)

def expected_prefix_last(name, actor_index, processed):
    if processed == 0:
        return 0
    if name == "source-actor-fifo-ten.ores":
        value = 0
        for n in range(1, processed + 1):
            value = value * 11 + n
        return value
    if name == "source-actor-scoped-owned-i64.ores":
        value = 0
        for n in range(1, processed + 1):
            value = 2 * (value + n)
        return value
    if name == "source-actor-interleaved-stop.ores":
        sequences = ([1], [3, 4])
        return sum(sequences[actor_index][:processed])
    return sum([1, 2][:processed])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--clang", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--sanitize", action="store_true")
    args = ap.parse_args()
    root = pathlib.Path(args.root)
    fixtures = root / "tests" / "fixtures"
    runtime = root / "src" / "main" / "cpp" / "oreslang" / "runtime"
    source_dirs = root / "src" / "main" / "cpp"
    positives = [
        ("source-actor-spawn-stop.ores", 1, 0, None),
        # Verify real compiled guest execution, not just IR text admission,
        # for all three source spellings of native instance on_message.
        ("source-actor-method-keyword.ores", 1, 2, 3),
        ("source-actor-bare-method.ores", 1, 2, 3),
        ("source-actor-send.ores", 1, 2, 3),
        ("source-actor-typed-ref.ores", 1, 2, 3),
        ("source-actor-await-ready.ores", 1, 2, 3),
        ("source-actor-five-workers.ores", 5, 2, 3),
        ("source-actor-fifo-ten.ores", 1, 10, 2853116705),
        ("source-actor-scoped-owned-i64.ores", 1, 8, 1004),
        ("source-actor-interleaved-stop.ores", 2, None, None),
        ("source-actor-send-moved-value.ores", 1, 2, 3),
        ("source-actor-send-reborrow-then-move.ores", 1, 2, 3),
    ]
    negatives = {
        "source-actor-owned-unknown-initializer-reject.ores":
            "unknown parameter 'not_yet_declared'",
        "source-actor-owned-forward-reference-reject.ores":
            "unknown parameter 'second'",
        "source-actor-unflagged-owned.ores":
            "experimental owned int requires --experimental-owned-int",
        "source-actor-scoped-owned-escape-reject.ores":
            "native actor owned temporary return requires pure scalar expression",
        "source-actor-static-method-reject.ores": "static method is invalid",
        "source-actor-typed-field-reject.ores": "native final actor fields require typed field initialization and ownership lowering",
        "source-actor-spawn-missing-stop.ores": "actor reference must be stopped",
        "source-actor-typed-ref-mismatch.ores": "ActorRef<T> type mismatch",
        "source-actor-typed-ref-nonspawn.ores": "ActorRef<T> requires spawn initializer",
        "source-future-typed-unimplemented.ores": "Future<T> requires native continuation lowering",
        "source-actor-spawn-double-stop.ores": "invalid or repeated actor stop",
        "source-actor-send-after-stop.ores": "actor message sent after stop",
        "source-actor-spawn-unknown.ores": "unknown actor type",
        "source-actor-spawn-unavailable-await.ores": "await get_ready requires one source actor without suspended locals",
        "source-actor-await-ready-impure-result.ores": "await get_ready requires side-effect-free scalar return",
        "source-actor-await-spawn.ores": "native async/channel/select boundary unsupported",
        "source-actor-ref-local-shadow.ores": "scalar local shadows actor reference",
        "source-actor-spawn-escape.ores": "unknown parameter",
        "source-actor-send-missing-handler.ores": "native actor supports only on_message",
        "source-actor-send-unknown-ref.ores": "unknown actor message receiver",
        "source-actor-send-ref-as-payload.ores": "unknown parameter",
        "source-actor-send-after-take-reject.ores": "use of explicitly moved int",
        "source-actor-send-borrow-reject.ores": "borrowed reference may only be passed",
        "source-actor-send-return-after-take-reject.ores": "use of explicitly moved int",
        "source-actor-send-expression-after-take-reject.ores": "use of explicitly moved int",
        "source-actor-send-class-value-reject.ores": "ownership violation: class is non-Copy",
        "source-actor-send-moved-class-field-reject.ores": "ownership violation: use-after-move class field",
        "source-actor-send-field-take-reject.ores": "ownership violation: use-after-move class field",
        # Explicitly refuse source contracts that need typed actor field
        # ownership or suspension-aware continuation lowering.
        "source-actor-final-group-field-reject.ores": "native final actor fields require typed field initialization and ownership lowering",
        "source-actor-suspend-method-reject.ores": "native suspends actor method requires continuation lowering",
    }
    for name, diagnostic in negatives.items():
        flags = ["--experimental-owned-int"] if (
            name.startswith("source-actor-scoped-owned-")
            or name.startswith("source-actor-owned-")
        ) else []
        result = run([args.compiler, *flags, str(fixtures / name)])
        assert result.returncode != 0 and diagnostic in result.stderr, (name, diagnostic, result.stderr)
        assert not result.stdout.strip(), ("unsafe LLVM IR emitted for rejected source", name)
    print(f"PASS {len(negatives)} no-IR actor ownership and await boundary cases", flush=True)
    with tempfile.TemporaryDirectory(prefix="oreslang-native-source-actor-") as temp:
        for name, actors, count, last in positives:
            flags = ["--experimental-owned-int"] if name.startswith(
                "source-actor-scoped-owned-") else []
            guest = run([args.compiler, *flags, str(fixtures / name)])
            assert guest.returncode == 0, (name, guest.stderr)
            assert guest.stdout.startswith("; ModuleID"), name
            assert "oreslang_source_actor_spawn" in guest.stdout, name
            assert "oreslang_source_actor_stop" in guest.stdout, name
            if name not in {
                "source-actor-spawn-stop.ores",
            }:
                assert "oreslang_source_actor_send" in guest.stdout, name
            if name == "source-actor-scoped-owned-i64.ores":
                assert guest.stdout.count(
                    "call void @ores_actor_i64_heap_scoped_new(") == 2, name
                assert guest.stdout.count(
                    "call void @ores_actor_i64_heap_scoped_drop(") == 2, name
                # One actor-native heap for the retained field plus two
                # reusable temporary slots. Immediate stop may abandon queued
                # DATA, so runtime repeat-turn proof belongs to dedicated host
                # tests; this source gate pins the generated bounded reserve.
                assert "i64 3" in guest.stdout, (
                    name, "actor heap needs bounded local slot reserve")
            if name == "source-actor-await-ready.ores":
                assert "oreslang_source_actor_await_ready" in guest.stdout, name
                assert "ready_continuation" in guest.stdout, name
            if name == "source-actor-interleaved-stop.ores":
                # IR call ordering must reflect lexical source ordering.
                # Previous lowering grouped every send before every stop.
                first_stop = guest.stdout.find("call void @oreslang_source_actor_stop")
                final_send = guest.stdout.rfind("call void @oreslang_source_actor_send")
                assert 0 <= first_stop < final_send, (name, "source actor effects reordered")
            ll = pathlib.Path(temp, name + ".ll")
            ll.write_text(guest.stdout)
            exe = pathlib.Path(temp, name + ".guest")
            flags = (["-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined"]
                     if args.sanitize else ["-O1"])
            command = [
                args.clang, *flags, "-x", "ir", str(ll),
                "-x", "c", str(runtime / "ActorOwnedI64Heap.c"),
                str(runtime / "NativeActorScheduler.c"),
                str(runtime / "NativeOresVM.c"),
                str(runtime / "NativeActorFuture.c"),
                str(runtime / "SourceActorBootstrap.c"),
                str(runtime / "NativeExecutionContextStore.c"),
                str(runtime / "NativeContinuationStore.c"),
                str(runtime / "NativeAwaitTraceBridge.c"),
                str(runtime / "NativeLogicalTrace.c"),
                "-std=c11", "-pthread", "-I", str(source_dirs),
                "-DORESLANG_SOURCE_ACTOR_TESTING", "-o", str(exe)
            ]
            linked = run(command)
            assert linked.returncode == 0, (name, linked.stderr)
            executed = run([str(exe)])
            assert executed.returncode == 43, (name, executed.returncode, executed.stderr)
            vm_lines = [line for line in executed.stderr.splitlines()
                        if "SOURCE_ACTOR_VM_ROUTING_VERIFIED" in line]
            assert len(vm_lines) == 1 and (
                "domains=4 main=1 cold_carriers=0" in vm_lines[0]), (
                name, vm_lines)
            if name == "source-actor-await-ready.ores":
                trace_lines = [line for line in executed.stderr.splitlines()
                               if "SOURCE_AWAIT_READY_TRACE_PASS" in line]
                assert len(trace_lines) == 1, (name, trace_lines)
                assert "execution_id_persisted=1 fresh_resume=1 root_slot=63" in trace_lines[0], trace_lines[0]
            lines = [line for line in executed.stderr.splitlines()
                     if "SOURCE_ACTOR_SPAWN_STOP_VERIFIED" in line]
            assert len(lines) == actors, (name, lines)
            for i, line in enumerate(lines):
                assert "constructor_on_worker=1 released=1" in line, line
                match = re.search(
                    r"accepted=(\\d+) processed=(\\d+) abandoned=(\\d+) last=(-?\\d+)",
                    line)
                assert match is not None, (name, line)
                accepted, processed, abandoned, observed_last = map(
                    int, match.groups())
                expected_accepted = (
                    [1, 2][i]
                    if name == "source-actor-interleaved-stop.ores"
                    else count)
                assert accepted == expected_accepted, (
                    name, i, accepted, expected_accepted)
                assert 0 <= processed <= accepted, (name, i, processed, accepted)
                assert abandoned == accepted - processed, (
                    name, i, accepted, processed, abandoned)
                assert observed_last == expected_prefix_last(
                    name, i, processed), (
                    name, i, processed, observed_last)
            print("PASS native source actor:", name, "actors", actors, flush=True)
    print("PASS current-source actor lowering and physical heap reclamation")

if __name__ == "__main__":
    main()
