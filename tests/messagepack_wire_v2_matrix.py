#!/usr/bin/env python3
"""Exact Java/GraalVM↔native MessagePack v2 byte-vector comparison."""
import argparse
import json
import subprocess
from pathlib import Path

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True)
    p.add_argument("--vectors", required=True)
    args = p.parse_args()
    document = json.loads(Path(args.vectors).read_text(encoding="utf-8"))
    assert document["schema"] == "oreslang.messagepack.collections.v2"
    vectors = document["vectors"]
    assert len(vectors) == 6
    for vec in vectors:
        completed = subprocess.run(
            [args.binary, "--vector", vec["schema_id"]],
            check=True, capture_output=True, text=True)
        actual = completed.stdout.strip()
        assert actual == vec["hex"], (
            f"{vec['schema_id']} Java/Python/native mismatch:\n"
            f"expected {vec['hex']}\ngot      {actual}")
    negative = subprocess.run([args.binary,"--negative"], check=True,
                              capture_output=True, text=True)
    assert "PASS" in negative.stdout, negative.stdout
    print(f"PASS six exact Java/Python/native MessagePack v2 vectors and hostile inputs")

if __name__ == "__main__":
    main()
