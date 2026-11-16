#!/usr/bin/env python3
"""Exact compiler/LLVM IR/Clang guest regressions for directional pipelines.

This is a test contract, not evidence of execution until a CI runner
actually builds oreslang-llvmc and runs CTest with nonzero steps.
"""
import argparse
import pathlib
import subprocess
import tempfile

POSITIVE = {
    "forward-42": (
        "fnc add_three(int x):int{return x+3;}\n"
        "fnc twice(int x):int{return x*2;}\n"
        "pub fnc main():int{return 18 |> add_three |> twice;}\n", 42),
    "reverse-42": (
        "fnc add_three(int x):int{return x+3;}\n"
        "fnc twice(int x):int{return x*2;}\n"
        "pub fnc main():int{return twice <| add_three <| 18;}\n", 42),
    "reverse-call-arg-42": (
        "fnc add(int x,int y):int{return x+y;}\n"
        "pub fnc main():int{return add(2) <| 40;}\n", 42),
}
NEGATIVE = {
    "mixed-forward-reverse":
        ("fnc f(int x):int{return x;}pub fnc main():int{return 41 |> f <| 42;}",
         "E_PIPE_DIRECTION"),
    "mixed-reverse-forward":
        ("fnc f(int x):int{return x;}pub fnc main():int{return f <| 41 |> f;}",
         "E_PIPE_DIRECTION"),
    "continuous-forward":
        ("pub fnc main():int{return 41 pipe 42;}", "NATIVE_ACTOR_PIPE_UNSUPPORTED"),
    "continuous-reverse":
        ("pub fnc main():int{return 42 rpipe 41;}", "NATIVE_ACTOR_PIPE_UNSUPPORTED"),
    "no-fake-stream-capture":
        ("pub fnc main():int{ (let results: Stream<int> = Stream.buffered<int>(64)) "
         "rpipe readable;return 0;}", None),
    "no-trap-panic-swallow":
        ("pub fnc main():int{val x = trap panic Error(\"panic\");return 0;}", None),
    "no-rpipe-mixed":
        ("pub fnc main():int{return 41 pipe 42 rpipe 43;}", None),
    "malformed-reverse-value":
        ("fnc f(int x):int{return x;}pub fnc main():int{return f <| ;}", None),
}

def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=False, check=False, timeout=65)

def verify(compiler, clang):
    with tempfile.TemporaryDirectory(prefix="atto-rpipe-native-") as temp:
        root = pathlib.Path(temp)
        for name,(source,wanted) in POSITIVE.items():
            path=root/(name+".atto")
            path.write_text(source,encoding="utf-8")
            result=run([compiler,str(path)])
            if result.returncode or not result.stdout.startswith(b"; ModuleID"):
                raise AssertionError(f"{name}: compiler failed: {result.stderr!r}")
            ll=root/(name+".ll")
            ll.write_bytes(result.stdout)
            native=root/(name+".bin")
            link=run([clang,"-Wno-override-module","-x","ir",str(ll),"-o",str(native)])
            if link.returncode:
                raise AssertionError(f"{name}: Clang link failed: {link.stderr!r}")
            guest=run([str(native)])
            if guest.returncode != wanted:
                raise AssertionError(f"{name}: native exit {guest.returncode}, wanted {wanted}")
            print(f"PASS actual compiled LLVM native guest {name}: {wanted}",flush=True)
        for name,(source,diagnostic) in NEGATIVE.items():
            path=root/(name+".atto")
            path.write_text(source,encoding="utf-8")
            result=run([compiler,str(path)])
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError(f"{name}: invalid source emitted LLVM IR or succeeded")
            if diagnostic is not None and diagnostic.encode() not in result.stderr:
                raise AssertionError(f"{name}: wanted {diagnostic}, got {result.stderr!r}")
            print(f"PASS rejection before LLVM IR {name}",flush=True)

if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--compiler",required=True)
    parser.add_argument("--clang",required=True)
    args=parser.parse_args()
    verify(args.compiler,args.clang)
