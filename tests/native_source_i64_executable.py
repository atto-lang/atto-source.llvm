#!/usr/bin/env python3
"""REAL native LLVM compiler->Clang->process execution for POSIX-safe i64 subset.

Exit status only represents 0..255, thus only four of seven portable cases
can be observed losslessly. It would be false to attest full signed-i64 parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

BLOB = "bbc319c19ecd007109d33592e210091eb267e5a2"
BYTES_SHA256 = "39bddbe558ad1c336271f91e6403c4555f2f16a7ed33a3355ee897721b6ab02a"
CORPUS = Path(__file__).resolve().parents[1] / "contracts/v1/source-i64/portable-i64.v1.json"
ADMITTED = ("precedence","grouping","signed-multiplication","subtraction")
OTHERS = ("negative-result","max-minus-one","signed-minimum")
NEGATIVES = ("3/2","9223372036854775808")

def require(good,message):
    if not good:raise RuntimeError(message)

def run(argv):
    return subprocess.run(argv,stdin=subprocess.DEVNULL,capture_output=True,timeout=20,check=False)

def verify_cases():
    raw=CORPUS.read_bytes()
    require(len(raw)<=8192 and
            hashlib.sha1(b"blob "+str(len(raw)).encode()+b"\0"+raw).hexdigest()==BLOB and
            hashlib.sha256(raw).hexdigest()==BYTES_SHA256,
            "unreviewed portable source-i64 corpus")
    def nodups(pairs):
        d={}
        for k,v in pairs:
            require(k not in d,"duplicate fixture field")
            d[k]=v
        return d
    doc=json.loads(raw.decode("utf-8"),object_pairs_hook=nodups)
    require(type(doc) is dict and set(doc)=={"schema_version","status","scope","cases"},"unexpected corpus schema")
    require(doc["schema_version"]=="1.0.0" and doc["status"]=="reference-only-not-compiled-execution",
            "unknown fixture version or false execution claim")
    cases=doc["cases"]
    require(type(cases) is list and len(cases)==7,"missing corpus cases")
    require(tuple(c["id"] for c in cases)==ADMITTED+OTHERS,"case ID/order mismatch")
    for c in cases:
        require(type(c) is dict and set(c)=={"id","expression","expected"},"invalid case")
        require(type(c["expression"]) is str and len(c["expression"])<=256,"unbounded expression")
        require(type(c["expected"]) is str and int(c["expected"])>=-(1<<63) and int(c["expected"])<(1<<63),
                "invalid expected i64")
    return cases[:4]

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--compiler",type=Path,required=True)
    p.add_argument("--clang",type=Path,required=True)
    args=p.parse_args()
    for binary in (args.compiler,args.clang):
        require(binary.exists() and binary.is_file(),"required native compiler/tool unavailable")
    compiler=args.compiler.resolve()
    clang=args.clang.resolve()
    observations=[]
    compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest()
    clang_sha256=hashlib.sha256(clang.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="oreslang-llvm-source-i64-") as td:
        d=Path(td)
        src=d/"fixture.ores"
        ir=d/"fixture.ll"
        exe=d/"fixture"
        for case in verify_cases():
            expected=int(case["expected"])
            require(0<=expected<=255,"OS exit code cannot represent this case: "+case["id"])
            src.write_text("pub fnc main():int{return "+case["expression"]+";}\n",encoding="utf-8")
            compiled=run([str(compiler),str(src)])
            require(compiled.returncode==0 and compiled.stdout.startswith(b";") or
                    (compiled.returncode==0 and b"define i64 @__oreslang_guest_main(" in compiled.stdout),
                    "real LLVM compiler failed case "+case["id"]+" "+repr(compiled.stderr[:1024]))
            require(b"define i32 @main(" in compiled.stdout,"native OS entry wrapper is missing")
            ir.write_bytes(compiled.stdout)
            linked=run([str(clang),"-O0","-x","ir",str(ir),"-o",str(exe)])
            require(linked.returncode==0 and exe.is_file(),
                    "Clang failed to link actual LLVM IR: "+repr(linked.stderr[:1024]))
            observed=run([str(exe)])
            require(observed.returncode==expected and observed.stdout==b"" and observed.stderr==b"",
                    "native exit-code mismatch: "+case["id"]+" "+repr((observed.returncode,observed.stdout,observed.stderr)))
            observations.append({"case":case["id"],"observed_exit":observed.returncode,
                                 "source_sha256":hashlib.sha256(src.read_bytes()).hexdigest(),
                                 "llvm_ir_sha256":hashlib.sha256(compiled.stdout).hexdigest(),
                                 "native_executable_sha256":hashlib.sha256(exe.read_bytes()).hexdigest()})
        for expr in NEGATIVES:
            src.write_text("pub fnc main():int{return "+expr+";}\n",encoding="utf-8")
            rejected=run([str(compiler),str(src)])
            require(rejected.returncode!=0 and rejected.stdout==b"" and rejected.stderr,
                    "unsupported source generated native IR: "+expr)
    print(json.dumps({"kind":"native-llvm-compiled-os-exit-subset",
                      "source_corpus_blob":BLOB,"source_corpus_sha256":BYTES_SHA256,
                      "compiler_sha256":compiler_sha256,"clang_sha256":clang_sha256,
                      "executed_cases":len(observations),
                      "total_corpus_cases":7,"unexecuted_due_to_lossy_exit":list(OTHERS),
                      "skipped_tests":0,"observed":observations},
                     sort_keys=True))
    return 0

if __name__=="__main__":
    try:sys.exit(main())
    except (OSError,RuntimeError,ValueError,subprocess.SubprocessError) as e:
        print("LLVM native source-i64 conformance DENIED: "+str(e),file=sys.stderr)
        sys.exit(1)
