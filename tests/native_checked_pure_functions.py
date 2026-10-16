#!/usr/bin/env python3
"""Compile and execute opt-in checked native Oreslang scalar call graphs.

This is real guest-source -> oreslang-llvmc -> LLVM IR -> Clang -> host ABI.
It is deliberately *not* actor supervision, owned-resource cleanup, or a
recoverable guest exception. The opt-in mode admits pure i64 value functions
only; integer overflow must cause a process trap, never a wrapping result.
"""
import argparse
from itertools import product
import os
from pathlib import Path
import signal
import subprocess
import tempfile

LO = -(1 << 63)
HI = (1 << 63) - 1
BOUNDS = (LO, LO + 1, -3037000500, -2, -1, 0, 1, 2, 3037000500, HI - 1, HI)
SOURCE = """
fnc add(int a,int b):int{return a+b;}
fnc sub(int a,int b):int{return a-b;}
fnc mul(int a,int b):int{return a*b;}
fnc neg(int a):int{return -a;}
fnc intermediate(int a,int b):int{return (a+b)*3;}
fnc inc(int a):int{return a+1;}
fnc twice(int a):int{return inc(inc(a));}
"""
C_HOST = r"""
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int64_t add(int64_t,int64_t);
extern int64_t sub(int64_t,int64_t);
extern int64_t mul(int64_t,int64_t);
extern int64_t neg(int64_t);
extern int64_t intermediate(int64_t,int64_t);
extern int64_t twice(int64_t);
int main(int argc, char **argv) {
    if (argc != 4) return 98;
    const int64_t a = (int64_t)strtoll(argv[2], NULL, 10);
    const int64_t b = (int64_t)strtoll(argv[3], NULL, 10);
    int64_t value;
    if (!strcmp(argv[1], "add")) value = add(a,b);
    else if (!strcmp(argv[1], "sub")) value = sub(a,b);
    else if (!strcmp(argv[1], "mul")) value = mul(a,b);
    else if (!strcmp(argv[1], "neg")) value = neg(a);
    else if (!strcmp(argv[1], "intermediate")) value = intermediate(a,b);
    else if (!strcmp(argv[1], "twice")) value = twice(a);
    else return 99;
    printf("%" PRId64 "\n", value);
    return 0;
}
"""


def execute(argv, *, timeout=30):
    result = subprocess.run(argv, text=True, capture_output=True,
                            timeout=timeout, check=False)
    if result.returncode != 0:
        raise AssertionError("command failed: " + " ".join(argv[:3]) +
                             " stderr: " + result.stderr[:1500])
    return result.stdout


def core_limit():
    # A deliberately trapped guest must not write large crash/core files.
    if os.name == "posix":
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def checked_run(binary, operation, a, b, expected):
    process = subprocess.run([str(binary), operation, str(a), str(b)],
                             capture_output=True, text=True, timeout=10,
                             check=False, preexec_fn=core_limit if os.name == "posix" else None)
    if expected is None:
        # Explicit llvm.trap: exit(0), wraparound, and a random runtime
        # failure are not accepted as evidence of overflow detection.
        accepted = {signal.SIGILL, signal.SIGTRAP}
        if process.returncode >= 0 or -process.returncode not in accepted:
            raise AssertionError(
                "%s(%s,%s): expected overflow trap, got %d, stdout=%r stderr=%r"
                % (operation, a, b, process.returncode,
                   process.stdout[:150], process.stderr[:150]))
    elif process.returncode != 0 or process.stdout != str(expected) + "\n" or process.stderr:
        raise AssertionError(
            "%s(%s,%s): expected %s, status=%d stdout=%r stderr=%r"
            % (operation, a, b, expected, process.returncode,
               process.stdout[:150], process.stderr[:150]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    compiler = str(Path(args.compiler).resolve(strict=True))
    clang = str(Path(args.clang).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="ores-checked-native-functions-") as temp:
        root = Path(temp)
        source = root / "pure.ores"
        source.write_text(SOURCE, encoding="utf-8")
        ir = execute([compiler, "--target=host", "--experimental-checked-i64-functions", str(source)])
        for name in ("sadd", "ssub", "smul"):
            if "llvm." + name + ".with.overflow.i64" not in ir:
                raise AssertionError("missing real checked LLVM " + name + " intrinsic")
        if "llvm.trap" not in ir:
            raise AssertionError("checked native functions have no terminal overflow trap")
        ir_file = root / "pure.ll"
        ir_file.write_text(ir, encoding="utf-8")
        host = root / "host.c"
        host.write_text(C_HOST, encoding="utf-8")
        binary = root / "checked-guest"
        execute([clang, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                 str(host), str(ir_file), "-o", str(binary)])
        cases = 0
        operations = {"add": lambda a, b: a+b,
                      "sub": lambda a, b: a-b,
                      "mul": lambda a, b: a*b}
        for op, func in operations.items():
            for a, b in product(BOUNDS, repeat=2):
                exact = func(a, b)
                checked_run(binary, op, a, b, exact if LO <= exact <= HI else None)
                cases += 1
        for op, a, b, value in [
            ("neg", LO, 0, None),
            ("neg", LO + 1, 0, HI),
            ("neg", HI, 0, -HI),
            ("intermediate", 2, 3, 15),
            ("intermediate", HI, 1, None),
            ("intermediate", HI, -1, None),
            ("twice", 2, 0, 4),
            ("twice", HI - 2, 0, HI),
            ("twice", HI - 1, 0, None),
        ]:
            checked_run(binary, op, a, b, value)
            cases += 1
        negatives = [
            "fnc bad(int a):int{val x:int=a;return x;}",  # lexical locals gate
            "fnc bad(int a):int{defer some(a);return a;}",  # side effects gate
            "fnc bad(int a):int{return rt take(a);}",  # ownership gate
            "fnc bad(int a):int{return missing(a);}",  # unknown callee
            "fnc bad(int a):int{return bad(a);}",  # direct recursion
            "fnc first(int a):int{return second(a);}"
            "fnc second(int a):int{return first(a);}",  # indirect recursion
            "fnc inc(int a):int{return a+1;}"
            "fnc twice(int a):int{return inc(a,a);}",  # wrong call arity
            "fnc twice(int a):int{return a;}"
            "fnc twice(int b):int{return b;}",  # duplicate name/arity
        ]
        # Balanced, shallow expression with more than 2048 AST nodes.
        # This must fail the node budget, not merely the recursion-depth gate.
        leaves = ["a"] * 2050
        while len(leaves) > 1:
            leaves = ["(" + leaves[i] + "+" + leaves[i + 1] + ")"
                      for i in range(0, len(leaves) - 1, 2)] + (
                          leaves[-1:] if len(leaves) % 2 else [])
        negatives.append("fnc huge(int a):int{return " + leaves[0] + ";}")
        for i, code in enumerate(negatives):
            invalid = root / ("invalid-%d.ores" % i)
            invalid.write_text(code + "\n", encoding="utf-8")
            attempted = subprocess.run(
                [compiler, "--experimental-checked-i64-functions", str(invalid)],
                capture_output=True, text=True, timeout=15, check=False)
            if attempted.returncode != 1 or not attempted.stderr.startswith("oreslang-llvmc:"):
                raise AssertionError("effectful checked source was not rejected")
        # The opt-in modes have intentionally incompatible output ABIs.
        mixed = subprocess.run(
            [compiler, "--experimental-checked-i64",
             "--experimental-checked-i64-functions", str(source)],
            capture_output=True, text=True, timeout=15, check=False)
        if mixed.returncode != 1 or not mixed.stderr.startswith("oreslang-llvmc:"):
            raise AssertionError("mixed checked ABI flags were not rejected")
    print("CHECKED_PURE_NATIVE_GUEST_CASES=%d NEGATIVE_ADMISSION=%d NO_OWNER_OR_ACTOR_CLAIM=TRUE" %
          (cases, len(negatives) + 1))


if __name__ == "__main__":
    main()
