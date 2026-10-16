#!/usr/bin/env python3
"""Real Oreslang -> LLVM IR -> Clang checked-result ABI executable test.

The opt-in pure value-only ABI is i32 __oreslang_checked_v2_NAME_ARITY(
    i64 arguments..., i64* out).
0 = success and one committed output; 1 = arithmetic overflow with untouched
output; 2 = null output pointer. Nested failures do not trap or publish values.
This is NOT actor/owner cleanup evidence or an approved cross-org mirror.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

LO, HI = -(1 << 63), (1 << 63) - 1
BOUNDS = (LO, LO + 1, -3037000500, -2, -1, 0, 1, 2, 3037000500, HI - 1, HI)
SOURCE = """
pub fnc add(int a,int b):int{return a+b;}
pub fnc sub(int a,int b):int{return a-b;}
pub fnc mul(int a,int b):int{return a*b;}
pub fnc neg(int a):int{return -a;}
fnc inc(int a):int{return a+1;}
pub fnc twice(int a):int{return inc(inc(a));}
fnc combine(int a,int b):int{return a+b;}
pub fnc nested(int a,int b):int{return combine(inc(a),inc(b));}
pub fnc intermediate(int a,int b):int{return (a+b)*3;}
"""
HOST = r"""
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int32_t __oreslang_checked_v2_add_2(int64_t,int64_t,int64_t*);
extern int32_t __oreslang_checked_v2_sub_2(int64_t,int64_t,int64_t*);
extern int32_t __oreslang_checked_v2_mul_2(int64_t,int64_t,int64_t*);
extern int32_t __oreslang_checked_v2_neg_1(int64_t,int64_t*);
extern int32_t __oreslang_checked_v2_twice_1(int64_t,int64_t*);
extern int32_t __oreslang_checked_v2_nested_2(int64_t,int64_t,int64_t*);
extern int32_t __oreslang_checked_v2_intermediate_2(int64_t,int64_t,int64_t*);

/* Reproducible high-volume differential oracle. All reference arithmetic
   uses signed 128-bit intermediates: no UB or wrapped-result oracle. */
static uint64_t next_random(uint64_t* state) {
    *state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
static int within_i64(__int128 number) {
    return number >= (__int128)INT64_MIN && number <= (__int128)INT64_MAX;
}
static int verify_binary(int32_t (*fn)(int64_t,int64_t,int64_t*),
                         int64_t a,int64_t b,__int128 exact) {
    int64_t output = INT64_C(987654321);
    int32_t status = fn(a,b,&output);
    int in_range = within_i64(exact);
    if (status != (in_range ? 0 : 1)) return 0;
    if (output != (in_range ? (int64_t)exact : INT64_C(987654321))) return 0;
    return fn(a,b,NULL) == 2;
}
static int verify_unary(int32_t (*fn)(int64_t,int64_t*),
                        int64_t a,__int128 exact) {
    int64_t output = INT64_C(987654321);
    int32_t status = fn(a,&output);
    int in_range = within_i64(exact);
    if (status != (in_range ? 0 : 1)) return 0;
    if (output != (in_range ? (int64_t)exact : INT64_C(987654321))) return 0;
    return fn(a,NULL) == 2;
}
static int bulk_check(void) {
    uint64_t state = UINT64_C(0x7c39252dea9f44b1);
    int passed = 0;
    for (int i = 0; i < 2048; ++i) {
        uint64_t ra = next_random(&state), rb = next_random(&state);
        int64_t a,b;
        memcpy(&a,&ra,sizeof(a));
        memcpy(&b,&rb,sizeof(b));
        __int128 add = (__int128)a+b;
        __int128 sub = (__int128)a-b;
        __int128 mul = (__int128)a*b;
        __int128 x = (__int128)a+1;
        __int128 y = (__int128)b+1;
        __int128 twice = (__int128)a+2;
        int64_t intermediate = INT64_C(987654321);
        int32_t status = __oreslang_checked_v2_nested_2(a,b,&intermediate);
        int nested_ok = within_i64(x) && within_i64(y) && within_i64(x+y);
        if (status != (nested_ok ? 0 : 1)) return 10;
        if (intermediate != (nested_ok ? (int64_t)(x+y) : INT64_C(987654321))) return 11;
        if (!verify_binary(__oreslang_checked_v2_add_2,a,b,add)) return 12;
        if (!verify_binary(__oreslang_checked_v2_sub_2,a,b,sub)) return 13;
        if (!verify_binary(__oreslang_checked_v2_mul_2,a,b,mul)) return 14;
        if (!verify_unary(__oreslang_checked_v2_twice_1,a,
                          (within_i64(x) ? twice : (__int128)INT64_MAX+1))) return 15;
        if (__oreslang_checked_v2_nested_2(a,b,NULL) != 2) return 16;
        passed += 5;
    }
    printf("CHECKED_RESULT_BULK=%d_PASS\n", passed);
    return 0;
}


/* This entire matrix is checked inside one guest-host process per optimization.
   The exact-width oracle uses signed 128-bit values, never wrapped i64 math. */
static int boundary_check(void) {
    static const int64_t values[] = {
        INT64_MIN, INT64_MIN+1, INT64_C(-3037000500), INT64_C(-2),
        INT64_C(-1), INT64_C(0), INT64_C(1), INT64_C(2),
        INT64_C(3037000500), INT64_MAX-1, INT64_MAX
    };
    int cases = 0;
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        int64_t a = values[i];
        if (!verify_unary(__oreslang_checked_v2_neg_1,a,-(__int128)a)) return 21;
        if (!verify_unary(__oreslang_checked_v2_twice_1,a,(__int128)a+2)) return 22;
        cases += 2;
        for (size_t j = 0; j < sizeof(values)/sizeof(values[0]); ++j) {
            int64_t b = values[j];
            if (!verify_binary(__oreslang_checked_v2_add_2,a,b,(__int128)a+b)) return 23;
            if (!verify_binary(__oreslang_checked_v2_sub_2,a,b,(__int128)a-b)) return 24;
            if (!verify_binary(__oreslang_checked_v2_mul_2,a,b,(__int128)a*b)) return 25;
            __int128 x = (__int128)a+1;
            __int128 y = (__int128)b+1;
            __int128 intermediate = (__int128)a+b;
            int64_t out = INT64_C(987654321);
            int32_t status = __oreslang_checked_v2_nested_2(a,b,&out);
            int nested_ok = within_i64(x) && within_i64(y) && within_i64(x+y);
            if (status != (nested_ok ? 0 : 1) ||
                out != (nested_ok ? (int64_t)(x+y) : INT64_C(987654321))) return 26;
            if (__oreslang_checked_v2_nested_2(a,b,NULL) != 2) return 27;
            out = INT64_C(987654321);
            status = __oreslang_checked_v2_intermediate_2(a,b,&out);
            int intermediate_ok = within_i64(intermediate) &&
                                  within_i64(intermediate*3);
            if (status != (intermediate_ok ? 0 : 1) ||
                out != (intermediate_ok ? (int64_t)(intermediate*3) :
                       INT64_C(987654321))) return 28;
            if (__oreslang_checked_v2_intermediate_2(a,b,NULL) != 2) return 29;
            cases += 5;
        }
    }
    if (cases != 627) return 30;
    printf("CHECKED_RESULT_BOUNDARIES=%d_PASS\n", cases);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "bulk")) return bulk_check();
    if (argc == 2 && !strcmp(argv[1], "boundary")) return boundary_check();
    if (argc != 4) return 99;
    int64_t a = (int64_t)strtoll(argv[2], NULL, 10);
    int64_t b = (int64_t)strtoll(argv[3], NULL, 10);
    int64_t out = INT64_C(987654321);
    int32_t status;
    int64_t* address = !strcmp(argv[1], "null") ? NULL : &out;
    if (!strcmp(argv[1], "add") || !strcmp(argv[1], "null"))
        status = __oreslang_checked_v2_add_2(a,b,address);
    else if (!strcmp(argv[1], "sub"))
        status = __oreslang_checked_v2_sub_2(a,b,address);
    else if (!strcmp(argv[1], "mul"))
        status = __oreslang_checked_v2_mul_2(a,b,address);
    else if (!strcmp(argv[1], "neg"))
        status = __oreslang_checked_v2_neg_1(a,address);
    else if (!strcmp(argv[1], "twice"))
        status = __oreslang_checked_v2_twice_1(a,address);
    else if (!strcmp(argv[1], "nested"))
        status = __oreslang_checked_v2_nested_2(a,b,address);
    else if (!strcmp(argv[1], "intermediate"))
        status = __oreslang_checked_v2_intermediate_2(a,b,address);
    else return 98;
    printf("%" PRId32 " %" PRId64 "\n", status, out);
    return 0;
}
"""


def run(args):
    return subprocess.run(args, text=True, capture_output=True,
                          timeout=30, check=False)


def executable_case(binary, op, a, b, value):
    p = run([str(binary), op, str(a), str(b)])
    status = 0 if value is not None else 1
    observed = value if value is not None else 987654321
    expected = "%d %d\n" % (status, observed)
    if p.returncode != 0 or p.stdout != expected or p.stderr:
        raise AssertionError("%s(%s,%s): expected %r, got rc=%d stdout=%r stderr=%r" %
                             (op, a, b, expected, p.returncode, p.stdout, p.stderr))


def rejected(compiler, work, label, source, flags=None):
    file = work / (label + ".ores")
    file.write_text(source + "\n", encoding="utf-8")
    args = [compiler, "--target=host", "--experimental-checked-i64-result-functions"]
    if flags:
        args.extend(flags)
    p = run(args + [str(file)])
    if p.returncode != 1 or not p.stderr.startswith("oreslang-llvmc:") or p.stdout:
        raise AssertionError(label + ": invalid guest was not rejected: " + repr(p))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--clang", required=True)
    opt = ap.parse_args()
    compiler = str(Path(opt.compiler).resolve(strict=True))
    clang = str(Path(opt.clang).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="ores-checked-result-native-") as tmp:
        work = Path(tmp)
        source = work / "pure.ores"
        source.write_text(SOURCE, encoding="utf-8")
        p = run([compiler, "--target=host", "--experimental-checked-i64-result-functions",
                 str(source)])
        if p.returncode or p.stderr:
            raise AssertionError("Oreslang checked result compiler failed " + repr(p))
        ir = p.stdout
        for symbol in ("add_2", "sub_2", "mul_2", "neg_1", "twice_1", "nested_2"):
            if not re.search(r"(?m)^define\s+i32\s+@__oreslang_checked_v2_" +
                             symbol + r"\s*\(", ir):
                raise AssertionError("missing checked result callable " + symbol)
        # Only explicitly public functions are C-callable. Private
        # helpers remain defined for internal checked-result calls, but
        # must not leak as external ABI entrypoints.
        for helper in ("inc_1", "combine_2"):
            if not re.search(r"(?m)^define\s+private\s+i32\s+@__oreslang_checked_v2_" +
                             helper + r"\s*\(", ir):
                raise AssertionError("checked result private helper exported or absent: " + helper)
            if re.search(r"(?m)^define\s+i32\s+@__oreslang_checked_v2_" +
                         helper + r"\s*\(", ir):
                raise AssertionError("checked result leaked private helper: " + helper)
        for intrinsic in ("sadd", "ssub", "smul"):
            if "llvm." + intrinsic + ".with.overflow.i64" not in ir:
                raise AssertionError("missing signed overflow intrinsic " + intrinsic)
        if "llvm.trap" in ir:
            raise AssertionError("pure checked-result mode must not trap on arithmetic overflow")
        irfile = work / "pure.ll"
        irfile.write_text(ir, encoding="utf-8")
        host = work / "host.c"
        host.write_text(HOST, encoding="utf-8")
        passed = 0
        for optimization in ("0", "2", "3"):
            exe = work / ("guest-O" + optimization)
            compiled = run([clang, "-O" + optimization, "-std=c11",
                            "-Wall", "-Wextra", "-Werror", "-Wno-override-module", str(host),
                            str(irfile), "-o", str(exe)])
            if compiled.returncode:
                raise AssertionError("Clang could not link real guest IR: " + compiled.stderr)
            # One process validates 2,048 pseudorandom full-width input pairs
            # across five operations with a signed-128-bit reference oracle.
            stress = run([str(exe), "bulk"])
            if stress.returncode or stress.stdout != "CHECKED_RESULT_BULK=10240_PASS\n" or stress.stderr:
                raise AssertionError("checked-result bulk differential failed: " + repr(stress))
            passed += 10240
            # One compiled guest executable per optimization: no shell
            # process per arithmetic input, including overflowing boundaries.
            boundaries = run([str(exe), "boundary"])
            if boundaries.returncode or boundaries.stdout != (
                    "CHECKED_RESULT_BOUNDARIES=627_PASS\n") or boundaries.stderr:
                raise AssertionError("real guest boundary differential failed: " + repr(boundaries))
            passed += 627
            # NULL first: no arithmetic, callee or output publication.
            p = run([str(exe), "null", str(HI), "1"])
            if p.returncode or p.stdout != "2 987654321\n" or p.stderr:
                raise AssertionError("null output contract failed: " + repr(p))
            passed += 1
        negatives = {
            "owner": "fnc bad(int a):int{val x:int=a;return x;}",
            "borrow": "fnc bad(int a):int{return rt borrow(a);}",
            "effect": "fnc bad(int a):int{defer other(a);return a;}",
            "unknown": "fnc bad(int a):int{return missing(a);}",
            "recursive": "fnc bad(int a):int{return bad(a);}",
            "indirect-cycle": ("fnc x(int a):int{return y(a);}"
                               "fnc y(int a):int{return x(a);}"),
            "duplicate": ("fnc bad(int a):int{return a;}"
                          "fnc bad(int b):int{return b;}"),
            "named-arguments": ("fnc inc(int a):int{return a+1;}"
                                "fnc twice(int a):int{return inc(a:a);}"),
        }
        for label, code in negatives.items():
            rejected(compiler, work, label, code)
        rejected(compiler, work, "mixed-trap", SOURCE,
                 ["--experimental-checked-i64-functions"])
        rejected(compiler, work, "mixed-status", SOURCE,
                 ["--experimental-checked-i64"])
        # Invalid flag combinations fail before source is read or parsed.
        for option in ("--experimental-owned-int", "--experimental-rt-scalar-loop"):
            attempt = run([compiler, "--experimental-checked-i64-result-functions",
                           option, str(source)])
            if attempt.returncode != 2 or not attempt.stderr.startswith(
                    "checked scalar function modes cannot combine"):
                raise AssertionError("incompatible experimental mode was not rejected: " + repr(attempt))
    print("CHECKED_RESULT_GUEST_CASES=%d NEGATIVE_ADMISSION=%d RESULT_ABI_V2=PASS" %
          (passed, len(negatives)+2))


if __name__ == "__main__":
    main()
