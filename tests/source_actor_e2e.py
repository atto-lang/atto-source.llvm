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
    if name == "source-actor-two-owned-heaps.ores":
        value = 0
        start = 1 if actor_index == 0 else 5
        for n in range(start, start + processed):
            value = 2 * (value + n)
        return value
    if name == "source-actor-interleaved-stop.ores":
        sequences = ([1], [3, 4])
        return sum(sequences[actor_index][:processed])
    return sum([1, 2][:processed])

def llvm_function_body(ir, marker):
    definition = re.search(
        r"^define\s+[^\n{]*" + re.escape(marker) + r"[^\n{]*\{",
        ir,
        re.MULTILINE,
    )
    assert definition is not None, ("missing LLVM function definition", marker)
    start = definition.start()
    end = ir.find("\n}", definition.end())
    assert end >= 0, ("malformed LLVM function", marker)
    return ir[start:end + 2]

def parse_actor_receipt(line):
    """Strictly parse the complete current native stop receipt."""
    parts = line.split()
    marker = "SOURCE_ACTOR_SPAWN_STOP_VERIFIED"
    assert parts and parts[0] == marker, ("missing actor receipt marker", line)
    expected = {
        "constructor_on_worker", "released", "ready",
        "accepted", "processed", "abandoned", "last",
    }
    fields = {}
    for field in parts[1:]:
        key, sep, value = field.partition("=")
        assert sep and key in expected and key not in fields, (
            "invalid actor receipt field", field)
        signed = key == "last" and value.startswith("-")
        digits = value[1:] if signed else value
        assert digits and digits.isascii() and digits.isdecimal(), (
            "invalid integer actor receipt", field)
        fields[key] = int(value)
    assert set(fields) == expected, ("missing actor receipt fields", fields)
    assert fields["released"] == 1, ("native actor release not verified", fields)
    assert fields["constructor_on_worker"] in (0, 1), fields
    assert fields["ready"] in (0, 1), ("invalid readiness receipt", fields)
    # Immediate stop can win before initialization begins. It must never
    # claim readiness or execute DATA in that canceled startup incarnation.
    if fields["constructor_on_worker"] == 0:
        assert fields["ready"] == fields["processed"] == fields["last"] == 0, fields
    return fields

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
        ("source-actor-send-async.ores", 1, 1, None),
        ("source-actor-send-async-local.ores", 1, 1, None),
        ("source-actor-send-async-bool-local.ores", 1, 1, None),
        ("source-actor-send-async-future.ores", 1, 2, None),
        ("source-actor-send-async-future-local.ores", 1, 2, None),
        ("source-actor-typed-ref.ores", 1, 2, 3),
        ("source-actor-await-ready.ores", 1, 2, 3),
        ("source-actor-await-worker.ores", 1, 2, 3),
        ("source-actor-future-void-ready.ores", 1, 2, 3),
        ("source-actor-await-done.ores", 1, 0, None),
        ("source-actor-five-workers.ores", 5, 2, 3),
        ("source-actor-fifo-ten.ores", 1, 10, 2853116705),
        ("source-actor-scoped-owned-i64.ores", 1, 8, 1004),
        ("source-actor-two-owned-heaps.ores", 2, 4, None),
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
        "source-actor-await-unknown.ores": "await get_ready requires a live direct ActorRef receiver",
        "source-actor-await-get-done.ores": "actor effects after await get_done are unsupported",
        "source-actor-await-worker-twice.ores": "native source supports only one async await boundary",
        "source-actor-await-after-stop.ores": "await get_ready must precede post-await actor effects",
        "source-actor-await-non-hasfuture-scalar.ores": "await get_ready requires a live direct ActorRef receiver",
        "source-actor-future-void-unused.ores": "Future<void> readiness token must be awaited exactly once",
        "source-actor-future-void-wrong-name.ores": "native Future<void> await requires its declared readiness token",
        "source-actor-future-void-double-await.ores": "native source supports only one async await boundary",
        "source-actor-future-void-unspawned.ores": "Future<void> requires previously spawned ActorRef",
        "source-actor-future-void-shadow.ores": "Future<void> readiness token shadows a live owner",
        "source-actor-future-void-duplicate.ores": "native source supports only one Future<void> token",
        "source-actor-future-void-wrong-producer.ores": "Future<void> requires ActorRef.get_ready() or ActorRef.send_async(i64)",
        "source-actor-future-void-send.ores": "unknown parameter",
        "source-actor-future-void-await-worker-instead.ores": "native Future<void> await requires its declared readiness token",
        "source-actor-future-void-await-again.ores": "native source supports only one async await boundary",
        "source-actor-spawn-double-stop.ores": "invalid or repeated actor stop",
        "source-actor-send-after-stop.ores": "actor message sent after stop",
        "source-actor-spawn-unknown.ores": "unknown actor type",
        "source-actor-spawn-unavailable-await.ores": "await get_ready requires one source actor without suspended locals",
        "source-actor-await-ready-impure-result.ores": "await get_ready requires side-effect-free scalar return",
        "source-actor-await-done-missing-stop.ores": "await get_done requires stop before await",
        "source-actor-await-done-effect-after.ores": "actor effects after await get_done are unsupported",
        "source-actor-await-done-impure-result.ores": "await get_done requires side-effect-free scalar return",
        "source-actor-await-two-lifecycle.ores": "native source supports only one async await boundary",
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
        "source-actor-send-async-unawaited-reject.ores":
            "native send_async statement must be awaited directly or bound to Future<void>",
        "source-actor-send-async-method-reject.ores":
            "native send_async direct await requires trusted zero-argument main",
        "source-actor-send-async-future-unused-reject.ores":
            "Future<void> send_async token must be awaited exactly once",
        "source-actor-send-async-future-stop-before-await-reject.ores":
            "send_async Future target stopped before await",
        "source-actor-send-async-too-many-locals-reject.ores":
            "direct send_async await captures only up to 16 Copy int/bool locals",
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
            flags = ["--experimental-owned-int"] if name in {
                "source-actor-scoped-owned-i64.ores",
                "source-actor-two-owned-heaps.ores",
            } else []
            guest = run([args.compiler, *flags, str(fixtures / name)])
            assert guest.returncode == 0, (name, guest.stderr)
            assert guest.stdout.startswith("; ModuleID"), name
            assert "oreslang_source_actor_spawn" in guest.stdout, name
            assert "oreslang_source_actor_stop" in guest.stdout, name
            if name not in {
                "source-actor-spawn-stop.ores",
                "source-actor-await-done.ores",
                "source-actor-send-async.ores",
                "source-actor-send-async-local.ores",
                "source-actor-send-async-bool-local.ores",
                "source-actor-send-async-future.ores",
                "source-actor-send-async-future-local.ores",
            }:
                assert "oreslang_source_actor_send" in guest.stdout, name
            if name in {
                    "source-actor-send-async.ores",
                    "source-actor-send-async-local.ores",
                    "source-actor-send-async-bool-local.ores"}:
                start_calls = [line for line in guest.stdout.splitlines()
                               if "call i32 @oreslang_source_actor_send_async_start_checked(" in line]
                detach_calls = [line for line in guest.stdout.splitlines()
                                if "call i32 @oreslang_source_actor_detach_send_future_state_checked(" in line]
                wait_calls = [line for line in guest.stdout.splitlines()
                              if "call i32 @oreslang_source_actor_wait_detached_send_result(" in line]
                fail_cleanup_calls = [line for line in guest.stdout.splitlines()
                                      if "call void @oreslang_source_actor_stop(" in line]
                discard_calls = [line for line in guest.stdout.splitlines()
                                 if "call i32 @oreslang_source_actor_discard_send_future_checked(" in line]
                assert len(start_calls) == 1, (
                    name, "expected direct send_async to checked-submit one scheduler Future", start_calls)
                assert "call ptr @oreslang_source_actor_send_async_start(" not in guest.stdout, (
                    name, "aborting unchecked send_async start ABI still emitted")
                assert "direct.send_async.start.failed" in guest.stdout, (
                    name, "missing immediate send_async failure cleanup branch")
                assert len(fail_cleanup_calls) == 2, (
                    name, "start and detach failure branches must both reclaim actor",
                    fail_cleanup_calls)
                assert len(discard_calls) == 1, (
                    name, "detach handoff failure must consume the started Future", discard_calls)
                assert "future.detach.direct.send.failed" in guest.stdout, (
                    name, "missing direct detach failure cleanup branch")
                assert len(detach_calls) == 1, (
                    name, "expected direct send_async to detach through runtime-owned scalar state", detach_calls)
                assert len(wait_calls) == 1, (
                    name, "expected host entry to wait only on detached terminal result", wait_calls)
                assert ".send_async_continuation" in guest.stdout, (
                    name, "missing generated direct send_async continuation")
                assert "call i64 @oreslang_source_actor_await_send_future(" not in guest.stdout, (
                    name, "synchronous guest-stack Future await ABI still emitted")
                assert "call void @oreslang_source_actor_await_send_async(" not in guest.stdout, (
                    name, "legacy host-wait direct send_async ABI still emitted")
                continuation = llvm_function_body(
                    guest.stdout, ".send_async_continuation")
                stop_pos = continuation.find(
                    "call void @oreslang_source_actor_request_stop(")
                return_pos = continuation.rfind("ret i64")
                assert 0 <= stop_pos < return_pos, (
                    name, "STOP admission must precede final return in resumed state",
                    continuation)
                assert "detached.state.count.valid" in continuation, (
                    name, "detached continuation must validate runtime state word count")
                if name in {
                        "source-actor-send-async-local.ores",
                        "source-actor-send-async-bool-local.ores"}:
                    assert "retained.detached.slot" in continuation, (
                        name, "captured local must resolve from runtime-owned detached frame")
                    assert "detached.state.ptr.valid" in continuation, (
                        name, "nonempty detached frame must reject a null runtime state pointer")
                    assert "retained.detached.capture" in guest.stdout, (
                        name, "guest pre-await path must copy captured scalar before detaching")
                if name == "source-actor-send-async-bool-local.ores":
                    assert "retained.detached.bool.i1" in guest.stdout
                    assert "retained.detached.bool" in guest.stdout, (
                        name, "captured bool must be canonicalized to a 0/1 detached word")
            if name in {
                    "source-actor-send-async-future.ores",
                    "source-actor-send-async-future-local.ores"}:
                start_calls = [line for line in guest.stdout.splitlines()
                               if "call i32 @oreslang_source_actor_send_async_start_checked(" in line]
                detach_calls = [line for line in guest.stdout.splitlines()
                                if "call i32 @oreslang_source_actor_detach_send_future_state_checked(" in line]
                wait_calls = [line for line in guest.stdout.splitlines()
                              if "call i32 @oreslang_source_actor_wait_detached_send_result(" in line]
                send_calls = [line for line in guest.stdout.splitlines()
                              if "call void @oreslang_source_actor_send(" in line]
                request_stop_calls = [line for line in guest.stdout.splitlines()
                                      if "call void @oreslang_source_actor_request_stop(" in line]
                fail_cleanup_calls = [line for line in guest.stdout.splitlines()
                                      if "call void @oreslang_source_actor_stop(" in line]
                discard_calls = [line for line in guest.stdout.splitlines()
                                 if "call i32 @oreslang_source_actor_discard_send_future_checked(" in line]
                assert len(start_calls) == 1, (
                    name, "expected one eager checked send_async Future submission", start_calls)
                assert "call ptr @oreslang_source_actor_send_async_start(" not in guest.stdout, (
                    name, "aborting unchecked send_async start ABI still emitted")
                assert ".start.failed" in guest.stdout, (
                    name, "missing stored Future immediate-failure cleanup branch")
                assert len(fail_cleanup_calls) == 2, (
                    name, "stored start and detach failure branches must both reclaim actor",
                    fail_cleanup_calls)
                assert len(discard_calls) == 1, (
                    name, "stored detach failure must consume the started Future", discard_calls)
                assert "future.detach.send.failed" in guest.stdout, (
                    name, "missing stored detach failure cleanup branch")
                assert len(detach_calls) == 1, (
                    name, "expected stored Future<void> to detach through runtime-owned scalar state", detach_calls)
                assert len(wait_calls) == 1, (
                    name, "expected host entry to await detached terminal result", wait_calls)
                assert len(send_calls) == 1, (
                    name, "expected one post-await ordinary send", send_calls)
                assert len(request_stop_calls) == 1, (
                    name, "post-await STOP must be nonblocking continuation cleanup", request_stop_calls)
                assert ".send_future_continuation" in guest.stdout, (
                    name, "missing generated post-await send Future continuation")
                assert "call i64 @oreslang_source_actor_await_send_future(" not in guest.stdout, (
                    name, "synchronous guest-stack Future await ABI still emitted")
                start_pos = guest.stdout.find(
                    "call i32 @oreslang_source_actor_send_async_start_checked(")
                detach_pos = guest.stdout.find(
                    "call i32 @oreslang_source_actor_detach_send_future_state_checked(")
                assert 0 <= start_pos < detach_pos, (
                    name, "send_async Future start must precede detached continuation handoff")
                continuation = llvm_function_body(
                    guest.stdout, ".send_future_continuation")
                send_pos = continuation.find(
                    "call void @oreslang_source_actor_send(")
                stop_pos = continuation.find(
                    "call void @oreslang_source_actor_request_stop(")
                return_pos = continuation.rfind("ret i64")
                assert 0 <= send_pos < stop_pos < return_pos, (
                    name, "post-await actor effects must retain lexical order",
                    continuation)
                assert "detached.state.count.valid" in continuation, (
                    name, "stored Future continuation must validate state word count")
                if name == "source-actor-send-async-future-local.ores":
                    capture_pos = guest.stdout.find("retained.detached.capture")
                    assert 0 <= start_pos < capture_pos < detach_pos, (
                        name, "eager Future submission must precede local capture and detach")
                    assert "retained.detached.slot" in continuation, (
                        name, "stored Future local must resolve from detached runtime frame")
                    assert "detached.state.ptr.valid" in continuation, (
                        name, "nonempty stored Future frame must reject a null runtime state pointer")
            if name in {
                "source-actor-scoped-owned-i64.ores",
                "source-actor-two-owned-heaps.ores",
            }:
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
            if name in {"source-actor-await-ready.ores",
                        "source-actor-await-worker.ores",
                        "source-actor-future-void-ready.ores"}:
                assert "oreslang_source_actor_await_ready" in guest.stdout, name
                assert "ready_continuation" in guest.stdout, name
                # A runtime declaration or source text alone is not an await:
                # require exactly one generated call to the CONTROL bridge.
                calls = [line for line in guest.stdout.splitlines()
                         if "call i64 @oreslang_source_actor_await_ready(" in line]
                assert len(calls) == 1, (name, "expected one guest await", calls)
            if name == "source-actor-await-done.ores":
                assert "oreslang_source_actor_request_stop" in guest.stdout, name
                assert "oreslang_source_actor_await_done" in guest.stdout, name
                assert "done_continuation" in guest.stdout, name
                assert "call void @oreslang_source_actor_request_stop" in guest.stdout, name
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
            if name in {"source-actor-await-ready.ores",
                        "source-actor-await-worker.ores",
                        "source-actor-future-void-ready.ores"}:
                trace_lines = [line for line in executed.stderr.splitlines()
                               if "SOURCE_AWAIT_READY_TRACE_PASS" in line]
                assert len(trace_lines) == 1, (name, trace_lines)
                assert "execution_id_persisted=1 fresh_resume=1 root_slot=63" in trace_lines[0], trace_lines[0]
            if name in {
                    "source-actor-send-async.ores",
                    "source-actor-send-async-local.ores",
                    "source-actor-send-async-future.ores",
                    "source-actor-send-async-future-local.ores"}:
                trace_lines = [line for line in executed.stderr.splitlines()
                               if "SOURCE_DETACHED_SEND_FUTURE_PASS" in line]
                assert len(trace_lines) == 1, (name, trace_lines)
                assert (
                    "outcome=0 guest_stack_released=1 "
                    "host_settlement_observer=0 final_join=1"
                    in trace_lines[0]), trace_lines[0]
            if name == "source-actor-await-done.ores":
                trace_lines = [line for line in executed.stderr.splitlines()
                               if "SOURCE_AWAIT_DONE_TRACE_PASS" in line]
                assert len(trace_lines) == 1, (name, trace_lines)
                assert "terminal_ack=1 execution_id_persisted=1 fresh_resume=1 root_slot=63" in trace_lines[0], trace_lines[0]
            lines = [line for line in executed.stderr.splitlines()
                     if "SOURCE_ACTOR_SPAWN_STOP_VERIFIED" in line]
            assert len(lines) == actors, (name, lines)
            receipts = [parse_actor_receipt(line) for line in lines]
            for i, receipt in enumerate(receipts):
                accepted = receipt["accepted"]
                processed = receipt["processed"]
                abandoned = receipt["abandoned"]
                observed_last = receipt["last"]
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
            if name == "source-actor-two-owned-heaps.ores":
                assert [r["last"] for r in receipts] == [
                    expected_prefix_last(name, i, r["processed"])
                    for i, r in enumerate(receipts)
                ], (name, receipts)
            print("PASS native source actor:", name, "actors", actors, flush=True)
    print("PASS current-source actor lowering and physical heap reclamation")

if __name__ == "__main__":
    main()
