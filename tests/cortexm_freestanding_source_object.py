#!/usr/bin/env python3
"""Exercise compiler admission and ARM codegen, not merely an IR text fixture."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def run(args, **kwargs):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, **kwargs)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    a = p.parse_args()
    clang = shutil.which("clang")
    if not clang:
        raise SystemExit("clang missing: cannot certify Cortex-M object output")
    with tempfile.TemporaryDirectory(prefix="ores-cortexm-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "probe.ores"
        source.write_text("""pub fnc ores_embedded_probe(): int {
    return sum(40, 2);
}
fnc sum(int a, int b): int {
    return a + b;
}
""")
        cmd = [a.compiler, "--target=thumbv7m-none-eabi", str(source)]
        good = run(cmd)
        assert good.returncode == 0, good.stderr
        assert 'target triple = "thumbv7m-none-eabi"' in good.stdout, good.stdout[:500]
        assert 'ores_embedded_probe' in good.stdout
        assert 'define' in good.stdout and '@sum' in good.stdout
        ir = root / "guest.ll"
        ir.write_text(good.stdout)
        obj = root / "guest.o"
        compiled = run([clang, "--target=thumbv7m-none-eabi", "-mcpu=cortex-m3",
                        "-mthumb", "-O2", "-ffreestanding", "-x", "ir", "-c",
                        str(ir), "-o", str(obj)])
        assert compiled.returncode == 0, compiled.stderr
        raw = obj.read_bytes()
        assert raw[:4] == b"\x7fELF" and raw[4:6] == b"\x01\x01"
        assert int.from_bytes(raw[18:20], "little") == 40

        # No native OS main wrapper: Zephyr owns the entrypoint.
        source.write_text("pub fnc main(): int { return 42; }\n")
        bad = run(cmd)
        assert bad.returncode != 0 and not bad.stdout
        assert "cortex-m" in bad.stderr.lower(), bad.stderr

        # Only stateless scalar expressions are admitted.
        source.write_text("pub fnc ores_embedded_probe(): int { val int x: 42; return x; }\n")
        bad = run(cmd)
        assert bad.returncode != 0 and not bad.stdout
        source.write_text("pub fnc ores_embedded_probe(): int { return 42; }\n")
        bad = run([a.compiler, "--target=thumbv6m-none-eabi", str(source)])
        assert bad.returncode != 0 and not bad.stdout
        assert "target" in bad.stderr.lower()
        bad = run([a.compiler, "--target=thumbv7m-none-eabi",
                   "--experimental-owned-int", str(source)])
        assert bad.returncode != 0 and not bad.stdout
    print("Cortex-M: source IR/ARM object and negative admission checks passed.")


if __name__ == "__main__":
    main()
