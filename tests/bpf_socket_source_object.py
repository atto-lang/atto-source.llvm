#!/usr/bin/env python3
"""Require real Oreslang -> LLVM BPF IR -> EM_BPF object and fail-closed negatives."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def run(argv):
    return subprocess.run(argv, capture_output=True, text=True, timeout=60)


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--output-object", type=pathlib.Path)
    args = parser.parse_args()
    check(pathlib.Path(args.compiler).is_file(), "missing compiled Oreslang compiler")
    check(shutil.which(args.clang) is not None, "no Clang: BPF object test cannot pass")
    with tempfile.TemporaryDirectory(prefix="oreslang-bpf-") as tmp:
        root = pathlib.Path(tmp)
        src = root / "socket.ores"
        cmd = [args.compiler, "--target=bpfel", str(src)]

        def reject(text):
            src.write_text(text, encoding="utf-8")
            p = run(cmd)
            check(p.returncode != 0 and not p.stdout,
                  "unsupported BPF source was accepted or leaked partial IR: " + repr(text[:90]))

        src.write_text("pub fnc socket_filter(): int { return 65535; }\n", encoding="utf-8")
        good = run(cmd)
        check(good.returncode == 0, "Oreslang BPF compilation failed: " + good.stderr)
        ir = good.stdout
        for required in ('target triple = "bpfel"', 'section "socket"',
                         'section "license"', 'define i32 @ores_bpf_socket(',
                         'ret i32 65535'):
            check(required in ir, "BPF IR missing " + required)
        check("@main(" not in ir, "host main leaked into BPF")
        for forbidden in ("oreslang_owned_", "oreslang_source_actor_", "malloc", "abort"):
            check(forbidden not in ir, "native runtime leaked into BPF: " + forbidden)
        ll = root / "socket.ll"
        ll.write_text(ir, encoding="utf-8")
        obj = root / "socket.o"
        p = run([args.clang, "-target", "bpfel", "-O2", "-x", "ir", "-c",
                 str(ll), "-o", str(obj)])
        check(p.returncode == 0, "Clang BPF backend failed: " + p.stderr)
        raw = obj.read_bytes()
        check(len(raw) >= 64 and raw[:6] == b"\x7fELF\x02\x01",
              "object is not ELF64 little-endian")
        check(int.from_bytes(raw[18:20], "little") == 247, "object is not EM_BPF")
        readelf = shutil.which("readelf") or shutil.which("llvm-readelf")
        check(readelf is not None, "readelf unavailable; section proof cannot pass")
        p = run([readelf, "-W", "-S", str(obj)])
        check(p.returncode == 0 and "socket" in p.stdout and "license" in p.stdout,
              "EM_BPF ELF missing socket/license sections: " + p.stderr)

        reject("pub fnc socket_filter(): int { return 65536; }\n")
        reject("pub fnc socket_filter(): int { return -1; }\n")
        reject("fnc socket_filter(): int { return 42; }\n")
        reject("pub fnc socket_filter(int x): int { return x; }\n")
        reject("pub fnc main(): int { return 42; }\n")
        reject("pub fnc socket_filter(): int { return 2 + 3; }\n")
        reject("pub fnc socket_filter(): int { val x: int = 5; return x; }\n")
        reject("pub fnc socket_filter(): int { return other(); }\n"
               "fnc other(): int { return 5; }\n")
        reject("pub fnc socket_filter(): int { return 1; }\n"
               "fnc other(): int { return 5; }\n")
        reject("//comment\n" * 500 + "pub fnc socket_filter(): int { return 5; }\n")
        reject("pub fnc socket_filter(): int { return 1; }\n// " + "x" * 1000000)
        reject("define actor Worker as\nlet int x = 0;\nend\n"
               "pub fnc socket_filter(): int { return 1; }\n")
        p = run([args.compiler, "--target=bpfel", "--experimental-owned-int", str(src)])
        check(p.returncode != 0 and not p.stdout, "BPF admitted native heap experiment")
        if args.output_object:
            args.output_object.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(obj, args.output_object)
    print("BPF_SOURCE_ELF_PASS admitted IR, EM_BPF ELF, socket/license and negatives")


if __name__ == "__main__":
    main()
