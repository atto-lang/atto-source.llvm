#!/usr/bin/env python3
"""Fail-closed admission and real ARM ELF compilation, including -O-safe checks."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, timeout=90)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    a = p.parse_args()
    clang = shutil.which("clang")
    require(bool(clang), "Clang required for real Cortex-M ELF validation")

    with tempfile.TemporaryDirectory(prefix="ores-cortexm-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "probe.ores"
        cmd = [a.compiler, "--target=thumbv7m-none-eabi", str(source)]

        def reject(text, needle=None):
            source.write_text(text)
            bad = run(cmd)
            require(bad.returncode != 0 and not bad.stdout,
                    "unexpected Cortex-M admission for: " + text[:140])
            if needle is not None:
                require(needle in bad.stderr.lower(),
                        "expected failure diagnostic " + needle + ": " + bad.stderr)

        source.write_text("""pub fnc ores_embedded_probe(int a, int b): int {
    return sum(a * 3, b);
}
fnc sum(int a, int b): int {
    return a + b;
}
""")
        good = run(cmd)
        require(good.returncode == 0, good.stderr)
        require('target triple = "thumbv7m-none-eabi"' in good.stdout,
                "Cortex-M triple missing from IR")
        require("ores_embedded_probe" in good.stdout and "@sum" in good.stdout,
                "guest symbols missing")
        require("define" in good.stdout, "IR has no guest definitions")
        require("define i32 @main" not in good.stdout, "unexpected host entrypoint")

        ir = root / "guest.ll"
        ir.write_text(good.stdout)
        obj = root / "guest.o"
        compiled = run([clang, "--target=thumbv7m-none-eabi", "-mcpu=cortex-m3",
                        "-mthumb", "-O2", "-ffreestanding", "-x", "ir", "-c",
                        str(ir), "-o", str(obj)])
        require(compiled.returncode == 0, compiled.stderr)
        raw = obj.read_bytes()
        require(len(raw) >= 52 and raw[:4] == b"\x7fELF" and raw[4:6] == b"\x01\x01",
                "compiler output is not ELF32 little-endian")
        require(int.from_bytes(raw[18:20], "little") == 40,
                "compiler output is not ARM ELF")

        reject("pub fnc main(): int { return 42; }\n", "cortex-m")
        reject("pub fnc a(): int { val int x: 42; return x; }\n", "cortex-m")
        reject("pub fnc recur(): int { return recur(); }\n", "recursive")
        reject("fnc a(): int { return b(); }\nfnc b(): int { return a(); }\n",
               "recursive")
        reject("fnc a(): int { return 1; }\nfnc a(int x): int { return x; }\n",
               "overloaded")
        reject("fnc f(): int { return " + "-" * 40 + "1; }\n", "budget")
        reject("".join("fnc f%d(): int { return %d; }\n" % (i, i)
                       for i in range(65)), "function count")
        reject("".join("fnc f%d(): int { return f%d(); }\n" % (i, i + 1)
                       for i in range(17)) + "fnc f17(): int { return 1; }\n",
               "call depth")
        reject("fnc f(" + ", ".join("int x%d" % i for i in range(9)) +
               "): int { return x0; }\n", "parameter budget")
        reject("// comment\n" * 10000 + "fnc f(): int { return 1; }\n",
               "source exceeds")
        source.write_text("fnc f(): int { return 1; }\n")
        bad = run([a.compiler, "--target=thumbv6m-none-eabi", str(source)])
        require(bad.returncode != 0 and not bad.stdout and "target" in bad.stderr.lower(),
                "unsupported target admitted")
        bad = run([a.compiler, "--target=thumbv7m-none-eabi",
                   "--experimental-owned-int", str(source)])
        require(bad.returncode != 0 and not bad.stdout,
                "owned-int experimental runtime admitted")
    print("Cortex-M scalar ABI, ARM ELF, recursion and bounded-admission tests PASS")


if __name__ == "__main__":
    main()
