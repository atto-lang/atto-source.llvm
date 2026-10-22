#!/usr/bin/env python3
import pathlib, subprocess, sys, tempfile

CASES = {
    "explicit": "fnc take(tuple (int,int,string?) value): int { return 1; }\n",
    "shorthand": "fnc take((int,int,string?) value): int { return 1; }\n",
    "structural": "fnc take(structural (int,int,string?) value): int { return 1; }\n",
    "return": "fnc take(): tuple (int,int) { return 1; }\n",
}
REQUIRED = {
    "explicit": "native positional tuple parameter requires verified readonly-slot and optional-tail lowering",
    "shorthand": "native positional tuple parameter requires verified readonly-slot and optional-tail lowering",
    "structural": "native structural positional tuple requires verified readonly-width and nonescape lowering",
    "return": "native positional tuple return requires verified tuple ABI lowering",
}

def run(compiler, path):
    return subprocess.run([str(compiler), str(path)], capture_output=True, text=True,
                          timeout=15, check=False)

def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: native_tuple_v2_gate.py <oreslang-llvmc>")
    compiler=pathlib.Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="ores-tuple-v2-") as root:
        root=pathlib.Path(root)
        good=root/"good.ores"
        good.write_text("pub fnc main(): int { return 42; }\n", encoding="utf-8")
        ok=run(compiler,good)
        if ok.returncode != 0 or "define i64 @__oreslang_guest_main(" not in ok.stdout:
            raise RuntimeError("tuple-v2 gate lacks a working scalar compiler")
        for name,source in CASES.items():
            path=root/(name+".ores")
            path.write_text(source,encoding="utf-8")
            before={p.name:p.read_bytes() for p in root.iterdir() if p.is_file()}
            result=run(compiler,path)
            after={p.name:p.read_bytes() for p in root.iterdir() if p.is_file()}
            if before != after:
                raise RuntimeError(name+": rejected source modified/published files")
            if result.returncode <= 0:
                raise RuntimeError(name+": expected diagnosed compiler failure")
            if REQUIRED[name] not in result.stderr:
                raise RuntimeError(name+": wrong rejection diagnostic: "+result.stderr[:400])
            if result.stdout.strip():
                raise RuntimeError(name+": rejected tuple source leaked LLVM output")
    print("native tuple-v2 surfaces fail closed with compiler-owned diagnostics")

if __name__ == "__main__":
    main()
