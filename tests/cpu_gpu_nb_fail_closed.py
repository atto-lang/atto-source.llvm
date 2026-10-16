#!/usr/bin/env python3
"""Real LLVM compiler gate for unsupported CPU/GPU/nb execution domains.

A positive program must first compile to LLVM IR. Every registered negative
program must then fail without publishing any IR, even when a previous
successful compile has run in the same test process. This is rejection
conformance, NOT evidence of source-domain execution support.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate CPU/GPU conformance key: " + key)
        result[key] = value
    return result


def run(compiler, path):
    return subprocess.run([str(compiler), str(path)], capture_output=True,
                          text=True, timeout=20, check=False)


def main():
    compiler = Path(sys.argv[1]).resolve()
    corpus = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"),
                        object_pairs_hook=unique_object)
    if len(corpus) != 10 or any(not isinstance(k, str) or not isinstance(v, str)
                                for k, v in corpus.items()):
        raise RuntimeError("incomplete/invalid CPU/GPU/nb source corpus")
    with tempfile.TemporaryDirectory(prefix="oreslang-native-dop-") as root:
        path = Path(root) / "main.ores"
        path.write_text("pub fnc main():int{return 42;}\n", encoding="utf-8")
        control = run(compiler, path)
        if control.returncode or "define " not in control.stdout:
            raise RuntimeError("real LLVM positive source->IR control failed: " +
                               control.stderr)
        for name, source in corpus.items():
            path.write_text(source + "\n", encoding="utf-8")
            failed = run(compiler, path)
            if failed.returncode == 0 or failed.stdout.strip() or not failed.stderr.strip():
                raise RuntimeError(f"{name}: unsupported domain did not fail closed "
                                   f"(status={failed.returncode}, stdout={failed.stdout!r}, "
                                   f"stderr={failed.stderr!r})")
            print(name + ": rejected with no LLVM IR")
    print("10/10 LLVM CPU/GPU/nb execution-domain rejections passed")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: cpu_gpu_nb_fail_closed.py <oreslang-llvmc> <corpus.json>")
    main()
