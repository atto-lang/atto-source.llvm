#!/usr/bin/env python3
"""Compiler-backed LLVM negative source admission: no IR on retired operators."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace

ROOT=Path(__file__).resolve().parents[1]
CASES={
    "removed-modulo":"45d5794870e93074d9b11b90833a849e96aaa4c1",
    "removed-caret":"6889c7e83c8d94b906fbfe78723caee9580da6aa",
    "removed-double-caret":"7ee99948018e927cce74c9ad0edc1975e4251689",
}
def require(ok,message):
    if not ok: raise RuntimeError(message)

def reject_native_output(result, label):
    """Compiler-owned diagnosed rejection; transport crashes and leaked IR are not passes."""
    require(result.returncode == 1, "rejection must exit 1, not crash: " + label)
    require(result.stdout == b"", "rejected source emitted LLVM IR: " + label)
    require(isinstance(result.stderr, bytes) and 0 < len(result.stderr) <= 65536 and
            result.stderr.startswith(b"oreslang-llvmc: "),
            "rejection lacks bounded compiler-owned diagnostic: " + label)


def selftest():
    valid = SimpleNamespace(returncode=1, stdout=b"",
                            stderr=b"oreslang-llvmc: unsupported operator")
    reject_native_output(valid, "control")
    variants = [
        SimpleNamespace(returncode=0, stdout=b"", stderr=valid.stderr),
        SimpleNamespace(returncode=-9, stdout=b"", stderr=valid.stderr),
        SimpleNamespace(returncode=2, stdout=b"", stderr=valid.stderr),
        SimpleNamespace(returncode=1, stdout=b"; ModuleID = 'leak'", stderr=valid.stderr),
        SimpleNamespace(returncode=1, stdout=b"", stderr=b""),
        SimpleNamespace(returncode=1, stdout=b"", stderr=b"shell failed"),
        SimpleNamespace(returncode=1, stdout=b"", stderr=b"oreslang-llvmc: " + b"x"*65536)
    ]
    for index, bad in enumerate(variants):
        try:
            reject_native_output(bad, str(index))
        except RuntimeError:
            continue
        raise AssertionError("negative verifier accepted spoofed compiler outcome " + str(index))
    print("ATTO_LLVM_NEGATIVE_VERIFIER_SELFTEST_7_DENIED")


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--compiler",type=Path)
    parser.add_argument("--selftest",action="store_true")
    args=parser.parse_args()
    if args.selftest:
        require(args.compiler is None, "selftest must not invoke compiler")
        selftest()
        return
    require(args.compiler is not None, "--compiler is required")
    require(not args.compiler.is_symlink(),"symlinked native compiler")
    compiler=args.compiler.resolve(strict=True)
    require(compiler.is_file(),"missing native compiler")
    for label,digest in CASES.items():
        source=ROOT/"contracts/atto-scalar-v1/negative"/(label+".atto")
        require(not source.is_symlink() and source.is_file(),"missing/linked source")
        content=source.read_bytes()
        git_blob=hashlib.sha1(b"blob "+str(len(content)).encode()+b"\0"+content).hexdigest()
        require(git_blob==digest,"unreviewed removed-operator fixture")
        # Keep the negative compiler in a sealed, empty working directory.
        # A nonzero return code alone is insufficient if it leaks output files.
        with tempfile.TemporaryDirectory(prefix="atto-llvm-denied-") as work:
            r=subprocess.run([str(compiler),str(source)],cwd=work,
                             stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,timeout=15)
            reject_native_output(r,label)
            require(not list(Path(work).iterdir()),
                    "rejected compiler produced a filesystem artifact: "+label)
    print("ATTO_LLVM_REMOVED_OPERATORS_REJECTED:3")

if __name__=="__main__": main()
