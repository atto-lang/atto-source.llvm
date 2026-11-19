#!/usr/bin/env python3
"""Execute actual canonical C++ parser, validate narrow typed actor IR and rejections.

No dynamic MIR from untrusted input is treated as authorized codegen: the
caller must separately verify exact binary/source Git SHA and execute compiler.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "fixtures/m4/shared_actor_counter.atto"
EXPECTED_BLOB = "0f9aca24deb195d7f604a1bfb944e237629803f4"

def blob(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()

def run(exe, source, ok):
    p = subprocess.run([str(exe), str(source)], stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, timeout=20)
    if ok:
        assert p.returncode == 0, f"unexpected source rejection: {p.stderr}"
        assert not p.stderr, p.stderr
        obj = json.loads(p.stdout)
        assert obj["schema"] == "atto.canonical-checked-actor-ir.v0"
        assert obj["checker"] == "LLVM_PARSE_PROGRAM_OWNERSHIP_SCALAR_TYPES"
        assert obj["status"] == "CHECKED_RESTRICTED_ACTOR_SLICE"
        assert obj["actor"] == {
            "name": "Counter", "field": "total", "initial": 0,
            "parameter": "increment", "operation": "checked_add_i64_return_field"
        }
        assert obj["spawns"] == [
            {"ref":"first","kind":"Counter"}, {"ref":"second","kind":"Counter"}]
        assert obj["actions"] == [
            {"op":"send","ref":"first","value":7},
            {"op":"send","ref":"second","value":9},
            {"op":"send","ref":"first","value":8},
            {"op":"stop","ref":"first"},
            {"op":"stop","ref":"second"}
        ]
        assert obj["return_i64"] == 43
        assert obj["limits"] == {"max_actor_instances":64,"max_source_sends":1024}
        assert "rt_take" in obj["unsupported"] and "tenant_sandbox" in obj["unsupported"]
        return obj
    assert p.returncode == 2, (p.stdout, p.stderr)
    assert not p.stdout.strip(), "rejected C++ source emitted IR"
    assert "M5_CANONICAL_ACTOR_IR_REFUSED" in p.stderr
    return None

def main():
    assert len(sys.argv) == 2, "requires exact locally built checked exporter path"
    exe = Path(sys.argv[1])
    assert exe.is_file() and not exe.is_symlink()
    src = SOURCE.read_bytes()
    assert blob(src) == EXPECTED_BLOB
    data = run(exe, SOURCE, True)
    print("M5_CANONICAL_SOURCE_SHA256="+hashlib.sha256(src).hexdigest())
    with tempfile.TemporaryDirectory(prefix="m5-canonical-") as work:
        directory = Path(work)
        probes = [
            ("use_after_stop", "stop first;", "stop first;\n  first.send(2);"),
            ("direct_actor_state", "first.send(7);", "first.total = 7;"),
            ("unsupported_dynamic", "first.send(7);", "rt take first; first.send(7);"),
            ("missing_actor_teardown", "stop second;", ""),
            ("unsupported_await", "first.send(7);", "await first.send(7);"),
            ("unsupported_handler", "total = total + increment;",
             "total = total * increment;"),
            ("malformed_literal", "first.send(7);", "first.send(9223372036854775808);"),
            ("unknown_actor", "spawn Counter();", "spawn Missing();"),
            ("moved_return", "return 43;", "return first;"),
        ]
        text = src.decode("utf-8")
        for name, needle, replacement in probes:
            assert needle in text, name
            source = directory / (name+".atto")
            source.write_text(text.replace(needle, replacement, 1))
            run(exe, source, False)
            print("M5_CHECKED_MIR_REJECTED_"+name.upper()+"=PASS")
    print("M5_CANONICAL_TYPED_ACTOR_IR=PASSED")
    print("EVIDENCE_SCOPE=REAL_LLVM_FRONTEND_VALIDATED_NARROW_ACTOR_IR_NOT_FULL_HIR_MIR")
    return 0

if __name__=="__main__":
    try:
        sys.exit(main())
    except (AssertionError, ValueError, OSError, subprocess.TimeoutExpired) as err:
        print("M5_CANONICAL_MIR_GATE_FAILED: "+str(err),file=sys.stderr)
        sys.exit(1)
