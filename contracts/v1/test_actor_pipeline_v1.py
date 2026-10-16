#!/usr/bin/env python3
"""Small self-contained negative fixture gate for the routing v1 verifier."""
import copy
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent

def verify(data):
    with tempfile.TemporaryDirectory(prefix="actor-v1-") as folder:
        fixture = pathlib.Path(folder) / "fixture.json"
        fixture.write_text(json.dumps(data, ensure_ascii=True), encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(HERE / "verify_actor_pipeline_v1.py"), str(fixture)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
            check=False
        ).returncode == 0

def main():
    original = json.loads((HERE / "actor-pipeline-routing-v1.json").read_text(encoding="utf-8"))
    if not verify(original):
        raise AssertionError("canonical fixture rejected")
    def check(label, fn):
        changed = copy.deepcopy(original)
        fn(changed)
        if verify(changed):
            raise AssertionError("invalid routing contract accepted: " + label)
    check("duplicate actor", lambda x: x["graph"]["stages"][2].update(id="worker-a"))
    check("mismatched payload", lambda x: x["graph"]["connections"][0].update(payload="Wrong"))
    check("wrong key affinity", lambda x: x["operations"]["by_key"][0].update(target="worker-b"))
    check("wrong FNV digest", lambda x: x["operations"]["by_key"][0].update(fnv1a64_hex="0"*16))
    check("missing NUL golden key", lambda x: x["operations"]["by_key"].pop())
    check("NUL instance", lambda x: x["graph"]["stages"][1].update(id="bad" + chr(0)))
    check("invalid UTF-8 scalar", lambda x: x["operations"]["by_key"][3].update(key=chr(0xd800)))
    check("oversize key", lambda x: x["operations"]["by_key"][0].update(key="k"*4097))
    check("unknown destination", lambda x: x["graph"]["connections"][0].update(target="missing"))
    check("wrong RR order", lambda x: x["operations"]["forward_round_robin"].reverse())
    print("ORES_ACTOR_PIPELINE_CONTRACT_NEGATIVE_PASS cases=10")

if __name__ == "__main__":
    main()
