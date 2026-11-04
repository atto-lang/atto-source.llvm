#!/usr/bin/env python3
"""Source-only AttoLang 19-case corpus integrity; no compiler/parity claims."""
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
MANIFEST_BLOB="43c4d295a237e483f70722bd90ec10d273b73854"

def require(ok,message):
    if not ok: raise RuntimeError(message)

def no_duplicates(pairs):
    result={}
    for key,value in pairs:
        require(key not in result,"duplicate manifest key")
        result[key]=value
    return result

def regular(relative):
    parts=Path(relative).parts
    require(parts and not Path(relative).is_absolute() and
            all(x not in ("",".","..") for x in parts) and
            "\\" not in relative, "invalid source path")
    cursor=ROOT
    for x in parts:
        cursor=cursor/x
        require(not cursor.is_symlink(),"symlinked Atto source")
    require(cursor.is_file(),"missing or nonregular Atto source")
    return cursor

def blob(data):
    return hashlib.sha1(b"blob "+str(len(data)).encode("ascii")+b"\0"+data).hexdigest()

def main():
    raw=regular("contracts/atto-scalar-v1/cases.json").read_bytes()
    require(blob(raw)==MANIFEST_BLOB,"unreviewed scalar source corpus")
    corpus=json.loads(raw.decode("utf-8"),object_pairs_hook=no_duplicates)
    require(corpus.get("schema")=="attolang.scalar-core.v1" and
            corpus.get("evidence_status")=="NOT_EXECUTED" and
            corpus.get("compiler_routes")==["java","llvm","wasm","net","forge"],
            "incorrect or forged backend scope")
    cases=corpus.get("cases",[])
    require(len(cases)==19 and sum(c["kind"]=="positive" for c in cases)==10 and
            sum(c["kind"]=="negative" for c in cases)==9,
            "incomplete source polarity")
    ids=set()
    for case in cases:
        require(case["id"] not in ids,"duplicate scalar case")
        ids.add(case["id"])
        relative=case["path"]
        require(relative=="contracts/atto-scalar-v1/"+case["kind"]+"/"+case["id"]+".atto",
                "noncanonical source case location")
        data=regular(relative).read_bytes()
        require(0<len(data)<=8192 and blob(data)==case["git_blob_sha1"],
                "mutated scalar source: "+case["id"])
    print("ATTO_LLVM_SCALAR_19_SOURCE_ONLY_NOT_EXECUTED")

if __name__=="__main__": main()
