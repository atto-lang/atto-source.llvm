#!/usr/bin/env python3
"""Compile and EXECUTE every reviewed signed-i64 expression via native LLVM ABI.

Compiles real Oreslang source with exported portable_i64_probe(): int, then
links compiler-generated LLVM IR to a fixed C harness that prints its i64
return (no lossy OS exit status). This is actual compiler+Clang execution,
not 7 compilations counted as 7 guest executions.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

BLOB = "bbc319c19ecd007109d33592e210091eb267e5a2"
BYTES_SHA256 = "39bddbe558ad1c336271f91e6403c4555f2f16a7ed33a3355ee897721b6ab02a"
CORPUS = Path(__file__).resolve().parents[1] / "contracts/v1/source-i64/portable-i64.v1.json"
IDS = ("precedence","grouping","signed-multiplication","subtraction",
       "negative-result","max-minus-one","signed-minimum")
# Main strict negatives (edeec90ad): unsupported division, out-of-range literal
# and the reserved legacy '%' operator must fail before any IR is emitted.
NEGATIVES = ("3/2","9223372036854775808","7%3","-7%-3","(8%3)*2","-9223372036854775808%-1")
C_DRIVER = r"""#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
extern int64_t portable_i64_probe(void);
int main(void) {
    const int64_t result = portable_i64_probe();
    return printf("%" PRId64 "\n", result) < 0 ? 1 : 0;
}
"""

def require(condition, message):
    if not condition:raise RuntimeError(message)

def invoke(args, timeout=30):
    return subprocess.run(args,stdin=subprocess.DEVNULL,capture_output=True,
                          timeout=timeout,check=False)

def cases():
    data=CORPUS.read_bytes()
    digest=hashlib.sha1(b"blob "+str(len(data)).encode()+b"\0"+data).hexdigest()
    require(0<len(data)<=8192 and digest==BLOB and
            hashlib.sha256(data).hexdigest()==BYTES_SHA256,
            "unreviewed portable source-i64 corpus")
    def unique(pairs):
        result={}
        for k,v in pairs:
            require(k not in result,"duplicate JSON property: "+k)
            result[k]=v
        return result
    record=json.loads(data.decode("utf-8"),object_pairs_hook=unique,
                      parse_constant=lambda n: require(False,"non-JSON constant "+n))
    require(type(record) is dict and set(record)=={"schema_version","status","scope","cases"},
            "unexpected source-i64 corpus fields")
    require(record["schema_version"]=="1.0.0" and record["status"]=="reference-only-not-compiled-execution",
            "unsupported source-i64 corpus schema/status")
    values=record["cases"]
    require(type(values) is list and len(values)==7,"source-i64 case count mismatch")
    for i,c in enumerate(values):
        require(type(c) is dict and set(c)=={"id","expression","expected"},"invalid case shape")
        require(c["id"]==IDS[i],"unexpected case ID or order")
        require(type(c["expression"]) is str and
                re.fullmatch(r"[0-9+*() \-]{1,256}",c["expression"]) is not None,
                "invalid arithmetic source")
        require(type(c["expected"]) is str and
                re.fullmatch(r"-?(?:0|[1-9][0-9]*)",c["expected"]) is not None and
                -(1<<63)<=int(c["expected"])<(1<<63),
                "out-of-range signed i64 expectation")
    return values

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--compiler",type=Path,required=True)
    parser.add_argument("--clang",type=Path,required=True)
    parser.add_argument("--report",type=Path)
    args=parser.parse_args()
    for program in (args.compiler,args.clang):
        # CMake's find_program(clang-18) is a distro symlink on Ubuntu;
        # resolve(strict=True) below pins the real binary that is hashed.
        require(program.is_file(),
                "real compiler and Clang binaries are required")
    compiler=args.compiler.resolve(strict=True)
    clang=args.clang.resolve(strict=True)
    manifest=cases()
    observed=[]
    with tempfile.TemporaryDirectory(prefix="oreslang-llvm-i64-native-") as td:
        root=Path(td)
        source=root/"case.ores"
        ir=root/"case.ll"
        driver=root/"driver.c"
        exe=root/"probe"
        driver.write_text(C_DRIVER,encoding="ascii")
        for item in manifest:
            program="pub fnc portable_i64_probe():int{return "+item["expression"]+";}\n"
            source.write_text(program,encoding="utf-8")
            compiled=invoke([str(compiler),str(source)])
            require(compiled.returncode==0 and compiled.stderr==b"" and
                    re.search(rb"define i64 @portable_i64_probe\(",compiled.stdout) is not None,
                    "native LLVM compiler did not emit an i64 guest symbol: "+
                    item["id"]+" "+repr(compiled.stderr[:1024]))
            require(b"define i32 @main(" not in compiled.stdout,
                    "test source unexpectedly emitted an OS main wrapper")
            ir.write_bytes(compiled.stdout)
            linked=invoke([str(clang),"-O1","-x","ir",str(ir),"-x","c",str(driver),
                           "-o",str(exe)])
            require(linked.returncode==0 and exe.is_file(),
                    "Clang failed to link real compiler IR: "+repr(linked.stderr[:1024]))
            process=invoke([str(exe)],timeout=10)
            expect=(item["expected"]+"\n").encode("ascii")
            require(process.returncode==0 and process.stdout==expect and
                    process.stderr==b"",
                    "full i64 native execution mismatch: "+item["id"]+
                    " "+repr((process.returncode,process.stdout,process.stderr)))
            observed.append({"id":item["id"],"signed_i64":item["expected"],
                             "source_sha256":hashlib.sha256(program.encode("utf-8")).hexdigest(),
                             "ir_sha256":hashlib.sha256(compiled.stdout).hexdigest(),
                             "linked_sha256":hashlib.sha256(exe.read_bytes()).hexdigest()})
        for expression in NEGATIVES:
            source.write_text("pub fnc portable_i64_probe():int{return "+expression+";}\n")
            rejected=invoke([str(compiler),str(source)])
            require(rejected.returncode==1 and rejected.stdout==b"" and
                    rejected.stderr.startswith(b"oreslang-llvmc: "),
                    "unexpected LLVM negative outcome (crash/noisy IR/no diagnostic): "+expression)
            if "%" in expression:
                require(b"'%' is reserved" in rejected.stderr,
                        "legacy percent rejection used unrelated compiler error: "+expression)
    receipt={"kind":"llvm-source-i64-full-width-native-compiled-execution",
             "source_corpus_git_blob":BLOB,"total_cases":len(manifest),
             "executed_cases":len(observed),"negative_cases":len(NEGATIVES),"skipped":0,
             "compiler_sha256":hashlib.sha256(compiler.read_bytes()).hexdigest(),
             "clang_sha256":hashlib.sha256(clang.read_bytes()).hexdigest(),
             "observed":observed}
    if args.report:
        args.report.write_text(json.dumps(receipt,sort_keys=True,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(receipt,sort_keys=True))
    return 0

if __name__=="__main__":
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError,UnicodeError,subprocess.SubprocessError) as exc:
        print("LLVM full-width portable i64 execution DENIED: "+str(exc),file=sys.stderr)
        sys.exit(1)
