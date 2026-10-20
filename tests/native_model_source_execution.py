#!/usr/bin/env python3
"""Real native define-model source -> LLVM IR -> Clang execution gate.

This proves only the current host i64 model slice. It does not prove stored
model values, float/string parity, freestanding execution, autograd, actors,
GPU placement or hard-real-time behavior.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

_MAX_CAPTURE = 2 * 1024 * 1024

CASES = {
    "default-config": ("""fnc Identity(int input): int { return input; }
fnc AddDelta(int delta, int input): int { return input + delta; }
pub define model PriceAdjust(delta: int = 2) as
    Identity()
    AddDelta(delta)
end
pub fnc main(): int { return PriceAdjust().run(40); }
""", 42),
    "explicit-config": ("""fnc Identity(int input): int { return input; }
fnc AddDelta(int delta, int input): int { return input + delta; }
pub define model PriceAdjust(delta: int = 2) as
    Identity()
    AddDelta(delta)
end
pub fnc main(): int { return PriceAdjust(3).run(39); }
""", 42),
    "negative-default": ("""fnc Identity(int input): int { return input; }
fnc AddDelta(int delta, int input): int { return input + delta; }
pub define model PriceAdjust(delta: int = -2) as
    Identity()
    AddDelta(delta)
end
pub fnc main(): int { return PriceAdjust().run(40); }
""", 38),
    "negative-explicit": ("""fnc Identity(int input): int { return input; }
fnc AddDelta(int delta, int input): int { return input + delta; }
pub define model PriceAdjust(delta: int = -2) as
    Identity()
    AddDelta(delta)
end
pub fnc main(): int { return PriceAdjust(-3).run(40); }
""", 37),
    "negative-static": ("""fnc AddDelta(int delta, int input): int { return input + delta; }
define model Fixed as
    AddDelta(-2)
end
pub fnc main(): int { return Fixed().run(5); }
""", 3),
}

REJECT = {
    "dynamic-config": ("""fnc Identity(int input): int { return input; }
define model Configured(delta: int) as
    Identity()
end
fnc use(int dynamic): int { return Configured(dynamic).run(1); }
""", "MODEL_NONCONSTANT_CONFIG"),
    "effectful-op": ("""fnc Helper(int input): int { return input; }
fnc HiddenCall(int input): int { return Helper(input); }
define model Bad as
    HiddenCall()
end
pub fnc main(): int { return 0; }
""", "MODEL_EFFECTFUL_BODY"),
    "recursive-composition": ("""define model A as
    B()
end
define model B as
    A()
end
pub fnc main(): int { return 0; }
""", "MODEL_RECURSIVE_COMPOSITION"),
}


def run(argv):
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        proc = subprocess.run(argv, stdout=out, stderr=err, timeout=65, check=False)
        out.seek(0, 2)
        err.seek(0, 2)
        if out.tell() > _MAX_CAPTURE or err.tell() > _MAX_CAPTURE:
            raise AssertionError("model compiler/tool emitted oversized output")
        out.seek(0)
        err.seek(0)
        return proc.returncode, out.read(), err.read()


def verify(compiler: Path, clang: Path):
    with tempfile.TemporaryDirectory(prefix="oreslang-model-guest-") as td:
        root = Path(td)
        for name, (source, expected) in CASES.items():
            src = root / f"{name}.ores"
            ll = root / f"{name}.ll"
            exe = root / name
            src.write_text(source, encoding="utf-8")
            rc, out, err = run([str(compiler), str(src)])
            if rc != 0 or not out.startswith(b"; ModuleID"):
                raise AssertionError(f"{name}: compiler rc={rc}, stderr={err!r}")
            if b"@Identity" not in out or b"@AddDelta" not in out or b"call i64 @AddDelta" not in out:
                raise AssertionError(f"{name}: expected lowered operation chain in LLVM IR")
            ll.write_bytes(out)
            rc, _, err = run([str(clang), "-Wno-override-module", "-x", "ir", str(ll), "-o", str(exe)])
            if rc != 0:
                raise AssertionError(f"{name}: clang rc={rc}, stderr={err!r}")
            rc, _, err = run([str(exe)])
            if rc != expected:
                raise AssertionError(f"{name}: guest exit={rc}, expected={expected}, stderr={err!r}")
            print(f"PASS model guest {name}: {expected}", flush=True)

        for name, (source, diagnostic) in REJECT.items():
            src = root / f"{name}.ores"
            src.write_text(source, encoding="utf-8")
            rc, out, err = run([str(compiler), str(src)])
            if rc == 0 or rc < 0 or out:
                raise AssertionError(f"{name}: rejection must occur before LLVM IR; rc={rc}")
            if diagnostic.encode() not in err:
                raise AssertionError(f"{name}: expected {diagnostic!r}, stderr={err!r}")
            print(f"PASS model reject {name}: {diagnostic}", flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", type=Path, required=True)
    ap.add_argument("--clang", type=Path, required=True)
    ns = ap.parse_args()
    verify(ns.compiler.resolve(), ns.clang.resolve())
