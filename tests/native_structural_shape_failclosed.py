#!/usr/bin/env python3
"""Actual oreslang-llvmc boundary: unsupported structural typing emits no LLVM IR.

Rejection is required until typed aggregate metadata, readonly loans, and
runtime shape dispatch are lowered. Source-free/mock results cannot certify it.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    probes = {
        "structural interface parameter":
            "define interface HasId\n id:int;\nend\n"
            "fnc take(structural HasId v):int{return v.id;}\n",
        "inline structural record parameter":
            "fnc take(structural { id:int } v):int{return v.id;}\n",
        "named shape pattern":
            "fnc main():int { match 1 over on structural HasId v -> "
            "{ return 1; } on _ -> { return 0; } end }\n",
        "inline shape pattern":
            "fnc main():int { match 1 over on structural { id:int } v -> "
            "{ return 1; } on _ -> { return 0; } end }\n",
        "structural tuple parameter":
            "fnc take(structural Tuple<int,string> v):int{return v[0];}\n",
        "structural tuple pattern":
            "fnc main():int { match 1 over on structural (int,int) v -> "
            "{ return 1; } on _ -> { return 0; } end }\n",
        "structural actor param":
            "actor fnc take(structural { id:int } v):int{return 1;}\n",
    }
    with tempfile.TemporaryDirectory(prefix="oreslang-native-structural-gate-") as tmp:
        cwd = Path(tmp)
        source = cwd / "probe.ores"
        def run(src):
            source.write_text(src, encoding="utf-8")
            result = subprocess.run([args.compiler, str(source)],
                                    cwd=cwd, capture_output=True, text=True,
                                    timeout=20, check=False)
            if source.read_text(encoding="utf-8") != src:
                raise AssertionError("compiler modified input fixture")
            extra = [x.name for x in cwd.iterdir() if x.name != "probe.ores"]
            if extra:
                raise AssertionError(("compiler staged unexpected artifact", extra))
            return result
        control = run("pub fnc main(): int { return 42; }\n")
        if control.returncode != 0 or not control.stdout.startswith("; ModuleID"):
            raise AssertionError(("native scalar positive control failed",
                                  control.returncode, control.stdout, control.stderr))
        for name, source_code in probes.items():
            result = run(source_code)
            if result.returncode == 0:
                raise AssertionError(("unsupported structural program compiled", name))
            if result.stdout.strip():
                raise AssertionError(("rejected structural program emitted LLVM IR", name))
            if not result.stderr.strip():
                raise AssertionError(("rejected structural program lacked diagnostic", name))
    print(f"native structural source admission: 1 scalar positive, {len(probes)} negative no-IR cases")

if __name__ == "__main__":
    main()
