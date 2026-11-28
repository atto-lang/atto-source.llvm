#!/usr/bin/env python3
"""Exact shared counted-select fixtures: rejection is not positive parity."""
import argparse, hashlib, pathlib, subprocess, tempfile

PINNED = {
  "counted-three.atto": "6531efa0689f2a5728b40a6412651e8143e45865",
  "counted-zero.atto": "42e49fc1eea64b31e86ec722e1457c1ab706a7e1",
  "counted-one.atto": "910062ebf96613d15a16ddb080abd89375d7709c",
  "counted-alias.atto": "324837078af3e7eeb9b619d0fb07142f0a0d6291",
  "reject-counted-negative.atto": "e2ff6fdb406b8807b74fa15beaee10a7f9438e22",
  "reject-counted-bool.atto": "4a52c22442bc8a9fe55eb95b23722595db32c97e",
  "reject-counted-scalar.atto": "a2a2dd184237f17b5e8d455f498719ef1ed01d97",
  "reject-counted-default.atto": "0c67c1fc8eaf55df6abb2b0fe7d2ce80f169bc69",
  "reject-counted-write.atto": "63fa3c9add56f3793e6d755f006cd3568acf1f04",
  "counted-dynamic.atto": "7c9ea9270d766612de0d502147e0df31b0dc3454",
  "counted-dynamic-zero.atto": "4707b13d3daebe05ced967418ba120cf801e2d8c",
  "reject-counted-dynamic-bool.atto": "36cd372a5385e34f2e3bd8dd936e4e0491052633",
  "reject-counted-dynamic-unknown.atto": "0e61da7a98987ed5a182148538b35b8a890abf26"
}

def run(compiler, source):
    return subprocess.run([compiler, str(source)], capture_output=True,
                          timeout=30, check=False)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    args = ap.parse_args()
    compiler = str(pathlib.Path(args.compiler).resolve())
    root = pathlib.Path(__file__).parent / "fixtures" / "counted-select-v1"
    for name, sha in PINNED.items():
        source = root / name
        raw = source.read_bytes()
        actual = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
        assert actual == sha, (name, actual, sha)
        result = run(compiler, source)
        assert result.returncode == 1 and not result.stdout.strip(), (name, result)
    # Isolate the select token from unsupported Channel construction and
    # suspends entrypoint syntax in the portable target fixtures.
    with tempfile.TemporaryDirectory() as td:
        path = pathlib.Path(td) / "probe.atto"
        for prefix in ("select", "nb select", "await select", "await nb select"):
            for header in ("(3)", "<int>(3)", "(0)", "(-1)", "(true)", "(1+2)", "(count)", "<int>(count)", "(count+1)", "<int>(count+1)"):
                for body in ("{ default: -> { return 42; } }", "over default: -> { return 42; } end"):
                    path.write_text("pub fnc main(): int { return " + prefix + header + " " + body + "; }\n")
                    result = run(compiler, path)
                    assert result.returncode == 1 and not result.stdout.strip(), (prefix, header, body, result)
                    assert b"native async/channel/select boundary unsupported" in result.stderr, result.stderr
        path.write_text("pub fnc main(): int { return 42; }\n")
        result = run(compiler, path)
        assert result.returncode == 0 and result.stdout.strip(), result.stderr
    print("LLVM_COUNTED_SELECT_ADMISSION_PASS 13 exact fixture blobs + 80 isolated rejections; no IR on rejection; positive counted guest NOT EXECUTED")

if __name__ == "__main__":
    main()
