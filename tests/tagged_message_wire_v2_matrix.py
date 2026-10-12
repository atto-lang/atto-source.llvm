#!/usr/bin/env python3
"""Executable nominal enum tag/identity parity against shared Java vector."""
import argparse
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--binary", required=True)
parser.add_argument("--vectors", required=True)
args = parser.parse_args()
contract = json.loads(Path(args.vectors).read_text(encoding="utf-8"))
assert contract["schema"] == "oreslang.tagged.message.v2"
actual = subprocess.check_output([args.binary], text=True).strip()
assert actual == contract["hex"], (
    "native enum message bytes differ from Java fixture: "
    + actual + " != " + contract["hex"])
print("PASS exact enum tag/identity typed MessagePack v2 vector")
