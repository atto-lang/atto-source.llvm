#!/usr/bin/env python3
"""Compiler-backed LLVM negative source admission: no IR on retired operators."""
import argparse
import hashlib
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
CASES={
    "removed-modulo":"45d5794870e93074d9b11b90833a849e96aaa4c1",
    "removed-caret":"6889c7e83c8d94b906fbfe78723caee9580da6aa",
    "removed-double-caret":"7ee99948018e927cce74c9ad0edc1975e4251689",
}
def require(ok,message):
    if not ok: raise RuntimeError(message)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--compiler",type=Path,required=True)
    args=parser.parse_args()
    require(not args.compiler.is_symlink(),"symlinked native compiler")
    compiler=args.compiler.resolve(strict=True)
    require(compiler.is_file(),"missing native compiler")
    for label,digest in CASES.items():
        source=ROOT/"contracts/atto-scalar-v1/negative"/(label+".atto")
        require(not source.is_symlink() and source.is_file(),"missing/linked source")
        content=source.read_bytes()
        git_blob=hashlib.sha1(b"blob "+str(len(content)).encode()+b"\0"+content).hexdigest()
        require(git_blob==digest,"unreviewed removed-operator fixture")
        r=subprocess.run([str(compiler),str(source)],stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=15)
        require(r.returncode==1 and r.stdout==b"" and
                b"ATTO_REMOVED_OPERATOR" in r.stderr and
                len(r.stderr)<=65536,
                "retired operator emitted IR or failed without diagnostic: "+label)
    print("ATTO_LLVM_REMOVED_OPERATORS_REJECTED:3")

if __name__=="__main__": main()
