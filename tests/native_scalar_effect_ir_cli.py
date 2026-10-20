#!/usr/bin/env python3
"""Check exact native source -> compiler -> scalar IR v1 contract (no network)."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile

GOLDEN = '{"schema_version":1,"functions":[{"name":"answer","operations":[{"kind":"compute","units":1},{"kind":"compute","units":1},{"kind":"compute","units":1}]}]}'
GOLDEN_SHA256 = "8ee1adbf6faef704c1eb6d2b2075c3b2d723097037f646f9e6e518a69d005463"


def run(binary, source, flags=()):
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary) / "source.ores"
        path.write_text(source, encoding="utf-8")
        return subprocess.run([binary, *flags, str(path)], capture_output=True,
                              text=True, check=False, timeout=20)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    arguments = parser.parse_args()
    assert hashlib.sha256(GOLDEN.encode()).hexdigest() == GOLDEN_SHA256
    positive = "fnc answer(): int { return 40 + 2; }\n"
    result = run(arguments.compiler, positive, ["--emit-scalar-effect-ir"])
    assert result.returncode == 0, result.stderr
    assert result.stdout == GOLDEN, (result.stdout, GOLDEN)
    program = json.loads(result.stdout)
    assert program["functions"][0]["operations"] == [
        {"kind": "compute", "units": 1}] * 3
    # Reject silent omission of unmodeled source-level effects and calls.
    negatives = [
        "fnc answer(): int { return read_external(); }\n",
        "fnc answer(): int { return -42; }\n",  # Negate not in v1 mapping
        "fnc answer(int x): int { return x + 1; }\n",
        "fnc answer(): int { return 1; }\nfnc answer(): int { return 2; }\n",
    ]
    for source in negatives:
        bad = run(arguments.compiler, source, ["--emit-scalar-effect-ir"])
        assert bad.returncode != 0 and bad.stdout == "", (bad.returncode, bad.stdout, bad.stderr)
    conflicting = run(arguments.compiler, positive,
                      ["--emit-scalar-effect-ir", "--tree-shake"])
    assert conflicting.returncode == 2 and conflicting.stdout == ""
    print("native scalar effect IR: compiler golden + negative controls PASS")


if __name__ == "__main__":
    main()
