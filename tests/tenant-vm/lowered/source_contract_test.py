#!/usr/bin/env python3
"""What the compiler must refuse about actors holding and messaging actors, and that it refuses it
without emitting IR. Each line of a program here is a way to get references wrong."""
import subprocess
import sys
import tempfile
from pathlib import Path

compiler = sys.argv[1]
WORKER = "define actor W as { let t: int = 0; pub fnc on_message(int n): int { t = t + n; return t; } }\n"
MAIN = "pub fnc main(): int { return 0; }\n"
ACCEPT_FLAGS = ["--target=wasm32", "--guest-abi=tenant-vm"]

ok = WORKER + "define actor P as { const c = spawn W(); pub fnc on_message(int n): int { c.send(n); return n; } }\n" + MAIN
cases = [
    # (name, source, flags, text the diagnostic must contain)
    ("needs the tenant VM ABI", ok, ["--target=wasm32"], "require --target=wasm32 --guest-abi=tenant-vm"),
    ("not on the native target", ok, [], "require --target=wasm32 --guest-abi=tenant-vm"),
    ("a reference is not a number", WORKER + "define actor P as { const c = spawn W(); pub fnc on_message(int n): int { return c + n; } }\n" + MAIN, ACCEPT_FLAGS, ""),
    ("a reference cannot be sent", WORKER + "define actor P as { const c = spawn W(); pub fnc on_message(int n): int { c.send(c); return n; } }\n" + MAIN, ACCEPT_FLAGS, ""),
    ("a reference cannot be returned", WORKER + "define actor P as { const c = spawn W(); pub fnc on_message(int n): int { return c; } }\n" + MAIN, ACCEPT_FLAGS, ""),
    ("a reference is not reassignable", WORKER + "define actor P as { const c = spawn W(); pub fnc on_message(int n): int { c = n; return n; } }\n" + MAIN, ACCEPT_FLAGS, ""),
    ("a reference must be const", WORKER + "define actor P as { let c = spawn W(); pub fnc on_message(int n): int { c.send(n); return n; } }\n" + MAIN, ACCEPT_FLAGS, "must be const"),
    ("an int field cannot be sent to", WORKER + "define actor P as { let c: int = 0; pub fnc on_message(int n): int { c.send(n); return n; } }\n" + MAIN, ACCEPT_FLAGS, "child reference field"),
    ("nor a parameter", WORKER + "define actor P as { const c = spawn W(); pub fnc on_message(int n): int { n.send(n); return n; } }\n" + MAIN, ACCEPT_FLAGS, "child reference field"),
    ("the declared type must match", WORKER + "define actor X as { let t: int = 0; pub fnc on_message(int n): int { return n; } }\ndefine actor P as { const c: ActorRef<X> = spawn W(); pub fnc on_message(int n): int { return n; } }\n" + MAIN, ACCEPT_FLAGS, "type mismatch"),
    ("unknown actor type", "define actor P as { const c = spawn Nope(); pub fnc on_message(int n): int { return n; } }\n" + MAIN, ACCEPT_FLAGS, "unknown actor type"),
    ("spawning itself", "define actor P as { const c = spawn P(); pub fnc on_message(int n): int { return n; } }\n" + MAIN, ACCEPT_FLAGS, "cycle"),
    ("spawning itself indirectly", "define actor A as { const b = spawn B(); pub fnc on_message(int n): int { return n; } }\ndefine actor B as { const a = spawn A(); pub fnc on_message(int n): int { return n; } }\n" + MAIN, ACCEPT_FLAGS, "cycle"),
]
failures = 0
with tempfile.TemporaryDirectory(prefix="tenant-vm-source-") as temporary:
    path = Path(temporary) / "case.atto"
    path.write_text(ok)
    accepted = subprocess.run([compiler, *ACCEPT_FLAGS, str(path)], capture_output=True, text=True, timeout=30)
    if accepted.returncode != 0 or "oreslang_vm_send_on_commit" not in accepted.stdout:
        print(f"  FAIL a valid program was refused: {accepted.stderr[:300]}")
        failures += 1
    for name, source, flags, wanted in cases:
        path.write_text(source)
        result = subprocess.run([compiler, *flags, str(path)], capture_output=True, text=True, timeout=30)
        if result.returncode == 0 or result.stdout:
            print(f"  FAIL accepted: {name}")
            failures += 1
        elif wanted not in result.stderr:
            print(f"  FAIL {name}: wanted {wanted!r}, got {result.stderr[:200]!r}")
            failures += 1
        elif "unordered_map" in result.stderr or result.returncode < 0:
            print(f"  FAIL {name}: crashed rather than diagnosed: {result.stderr[:200]!r}")
            failures += 1
print(f"  {len(cases)} wrong uses of references refused" if not failures else f"  {failures} failed")
print("SOURCE_CONTRACT_FAIL" if failures else "SOURCE_CONTRACT_PASS")
sys.exit(1 if failures else 0)
