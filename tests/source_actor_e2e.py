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


def parse_native_decimal(value, *, signed=False):
    """Only canonical, bounded ASCII integers printed by the native C runtime."""
    assert isinstance(value, str) and value, ("empty native integer", value)
    negative = signed and value.startswith("-")
    digits = value[1:] if negative else value
    # C prints uint64_t/int64_t. Reject giant strings before int conversion.
    assert digits and len(digits) <= 20 and digits.isascii() and digits.isdecimal(), (
        "non-decimal or oversized native integer", value)
    assert (len(digits) == 1 or digits[0] != "0") and (
        not negative or digits != "0"), ("noncanonical native integer", value)
    result = int(value)
    if signed:
        assert -(1 << 63) <= result <= (1 << 63) - 1, (
            "signed native integer overflow", value)
    else:
        assert 0 <= result <= (1 << 64) - 1, (
            "unsigned native integer overflow", value)
    return result


def parse_actor_receipt(line):
    """Strictly parse a complete native stop receipt, without prefix matches."""
    assert line.isascii(), ("non-ASCII native actor receipt", line)
    parts = line.split()
    marker = "SOURCE_ACTOR_SPAWN_STOP_VERIFIED"
    assert parts and parts[0] == marker and line == " ".join(parts), (
        "noncanonical actor receipt framing", line)
    expected_order = ("constructor_on_worker", "released", "processed", "last")
    expected = set(expected_order)
    fields = {}
    for field in parts[1:]:
        key, sep, value = field.partition("=")
        assert sep and key in expected and key not in fields, (
            "invalid actor receipt field", field)
        fields[key] = parse_native_decimal(value, signed=(key == "last"))
    assert tuple(fields) == expected_order, (
        "missing or reordered actor receipt fields", fields)
    assert fields["constructor_on_worker"] == 1 and fields["released"] == 1, (
        "native actor constructor/release not verified", fields)
    return fields


def parse_native_control_receipt(line, marker, expected, dynamic=()):
    """Require exact native VM/continuation evidence, never substring evidence."""
    assert line.isascii(), ("non-ASCII native control receipt", line)
    parts = line.split()
    assert parts and parts[0] == marker and line == " ".join(parts), (
        "noncanonical native control receipt framing", line)
    required_order = tuple(expected) + tuple(dynamic)
    required = set(required_order)
    fields = {}
    for part in parts[1:]:
        key, sep, value = part.partition("=")
        assert sep and key in required and key not in fields, (
            "duplicate or unexpected control receipt field", part)
        fields[key] = parse_native_decimal(value)
    assert tuple(fields) == required_order, (
        "missing or reordered native control receipt fields", fields)
    for key, value in expected.items():
        assert fields[key] == value, ("wrong native control receipt value", key, fields)
    return fields

def verify_native_owned_ir(ir, name):
    """Require two actual [3 x i64] ownership tokens and LIFO native drops."""
    # Match compiler-issued named stack tokens, not incidental "i64 3"
    # constants from a user message, initializer or unrelated instruction.
    token_pattern = (
        r"^\\s*%([A-Za-z_.$][A-Za-z0-9_.$]*)\\s*=\\s*alloca\\s+"
        r"\\[3 x i64\\](?:,\\s*align\\s+\\d+)?\\s*$"
    )
    tokens = re.findall(token_pattern, ir, flags=re.MULTILINE)
    assert len(tokens) == 2 and len(set(tokens)) == 2, (
        name, "missing or aliased three-word native ownership tokens", tokens)
    assert all(token.endswith(".actor.owned.token") for token in tokens), (
        name, "native token did not come from an owned actor local", tokens)

    def issued_tokens(symbol):
        calls = [
            line.strip() for line in ir.splitlines()
            if re.match(r"^\\s*call void @" + re.escape(symbol) + r"\\(", line)
        ]
        used = []
        for line in calls:
            token = re.search(
                r"\\bptr\\s+%([A-Za-z_.$][A-Za-z0-9_.$]*)\\)\\s*$", line)
            assert token, (name, "missing native ownership token argument", line)
            used.append(token.group(1))
        return used

    issued = issued_tokens("ores_actor_i64_heap_scoped_new")
    dropped = issued_tokens("ores_actor_i64_heap_scoped_drop")
    assert issued == tokens, (
        name, "native ownership constructor count, identity or order mismatch",
        issued, tokens)
    assert dropped == list(reversed(tokens)), (
        name, "native ownership drops must be exactly once in LIFO order",
        dropped, tokens)


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
            }:
                assert "oreslang_source_actor_send" in guest.stdout, name
            if name in {
                "source-actor-scoped-owned-i64.ores",
                "source-actor-two-owned-heaps.ores",
            }:
                verify_native_owned_ir(guest.stdout, name)
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
            assert len(vm_lines) == 1, (name, vm_lines)
            parse_native_control_receipt(
                vm_lines[0], "SOURCE_ACTOR_VM_ROUTING_VERIFIED",
                {"domains": 4, "main": 1, "cold_carriers": 0, "root_slot": 63})
            if name == "source-actor-await-ready.ores":
                trace_lines = [line for line in executed.stderr.splitlines()
                               if "SOURCE_AWAIT_READY_TRACE_PASS" in line]
                assert len(trace_lines) == 1, (name, trace_lines)
                parse_native_control_receipt(
                    trace_lines[0], "SOURCE_AWAIT_READY_TRACE_PASS",
                    {"execution_id_persisted": 1, "fresh_resume": 1,
                     "root_slot": 63}, dynamic=("source_token",))
            lines = [line for line in executed.stderr.splitlines()
                     if "SOURCE_ACTOR_SPAWN_STOP_VERIFIED" in line]
            assert len(lines) == actors, (name, lines)
            receipts = [parse_actor_receipt(line) for line in lines]
            for i, receipt in enumerate(receipts):
                if name == "source-actor-interleaved-stop.ores":
                    expected = [(1, 1), (2, 7)][i]
                    assert (receipt["processed"], receipt["last"]) == expected, (
                        name, receipt, expected)
                else:
                    assert receipt["processed"] == count, (name, receipt, count)
                    if last is not None:
                        assert receipt["last"] == last, (name, receipt, last)
            if name == "source-actor-two-owned-heaps.ores":
                # Two independent heap owners must report different results.
                # This is an executed-native-runtime assertion, not IR grep.
                assert sorted(receipt["last"] for receipt in receipts) == [
                    52, 172
                ], (name, receipts)
            print("PASS native source actor:", name, "actors", actors, flush=True)
    print("PASS current-source actor lowering and physical heap reclamation")

if __name__ == "__main__":
    main()
