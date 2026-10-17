#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import sys
import tempfile

def require(ok, message):
    if not ok:
        raise RuntimeError(message)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--clang", required=True)
    ap.add_argument("--fixture", required=True)
    args = ap.parse_args()

    compiled = subprocess.run(
        [args.compiler, args.fixture],
        capture_output=True, text=True, check=False, timeout=20)
    require(compiled.returncode == 0,
            "Oreslang complex128 fixture failed to compile: " + compiled.stderr)
    require("ret { double, double }" in compiled.stdout,
            "generated IR lacks typed complex128 aggregate return")
    require("define { double, double } @complex_add_sub()" in compiled.stdout,
            "complex_add_sub has the wrong LLVM ABI")
    require("define { double, double } @complex_negate()" in compiled.stdout,
            "complex_negate has the wrong LLVM ABI")
    require("define { double, double } @complex_conj()" in compiled.stdout,
            "complex_conj has the wrong LLVM ABI")
    require("define { double, double } @complex_conjugate()" in compiled.stdout,
            "complex_conjugate has the wrong LLVM ABI")
    require("define { double, double } @complex_imag_binary64_rounding()" in compiled.stdout,
            "complex_imag_binary64_rounding has the wrong LLVM ABI")
    require("define { double, double } @complex_imag_overflow()" in compiled.stdout,
            "complex_imag_overflow has the wrong LLVM ABI")
    require("define { double, double } @complex_imag_underflow()" in compiled.stdout,
            "complex_imag_underflow has the wrong LLVM ABI")
    require("define { double, double } @complex_imag_negative_underflow()" in compiled.stdout,
            "complex_imag_negative_underflow has the wrong LLVM ABI")
    require("define { double, double } @complex_imag_min_subnormal()" in compiled.stdout,
            "complex_imag_min_subnormal has the wrong LLVM ABI")

    harness = r'''
#include <stdint.h>
#include <string.h>

typedef struct {
    double real;
    double imaginary;
} ores_complex128;

extern ores_complex128 complex_add_sub(void);
extern ores_complex128 complex_negate(void);
extern ores_complex128 complex_conj(void);
extern ores_complex128 complex_conjugate(void);
extern ores_complex128 complex_imag_binary64_rounding(void);
extern ores_complex128 complex_imag_overflow(void);
extern ores_complex128 complex_imag_underflow(void);
extern ores_complex128 complex_imag_negative_underflow(void);
extern ores_complex128 complex_imag_min_subnormal(void);

static uint64_t bits(double value) {
    uint64_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    return raw;
}

int main(void) {
    ores_complex128 arithmetic = complex_add_sub();
    if (bits(arithmetic.real) != UINT64_C(0x3ff4000000000000)) return 11;
    if (bits(arithmetic.imaginary) != UINT64_C(0x3ff8000000000000)) return 12;

    ores_complex128 negated = complex_negate();
    if (bits(negated.real) != UINT64_C(0xbff0000000000000)) return 13;
    if (bits(negated.imaginary) != UINT64_C(0xc000000000000000)) return 14;

    ores_complex128 short_name = complex_conj();
    if (bits(short_name.real) != UINT64_C(0x4008000000000000)) return 15;
    if (bits(short_name.imaginary) != UINT64_C(0xc010000000000000)) return 16;

    ores_complex128 long_name = complex_conjugate();
    if (bits(long_name.real) != UINT64_C(0x4008000000000000)) return 17;
    if (bits(long_name.imaginary) != UINT64_C(0xc010000000000000)) return 18;

    ores_complex128 rounded_literal = complex_imag_binary64_rounding();
    if (bits(rounded_literal.real) != UINT64_C(0x0000000000000000)) return 19;
    if (bits(rounded_literal.imaginary) != UINT64_C(0x4340000000000000)) return 20;

    ores_complex128 overflow = complex_imag_overflow();
    if (bits(overflow.real) != UINT64_C(0x0000000000000000)) return 21;
    if (bits(overflow.imaginary) != UINT64_C(0x7ff0000000000000)) return 22;

    ores_complex128 underflow = complex_imag_underflow();
    if (bits(underflow.real) != UINT64_C(0x0000000000000000)) return 23;
    if (bits(underflow.imaginary) != UINT64_C(0x0000000000000000)) return 24;

    ores_complex128 negative_underflow = complex_imag_negative_underflow();
    if (bits(negative_underflow.real) != UINT64_C(0x8000000000000000)) return 25;
    if (bits(negative_underflow.imaginary) != UINT64_C(0x8000000000000000)) return 26;

    ores_complex128 min_subnormal = complex_imag_min_subnormal();
    if (bits(min_subnormal.real) != UINT64_C(0x0000000000000000)) return 27;
    if (bits(min_subnormal.imaginary) != UINT64_C(0x0000000000000001)) return 28;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="oreslang-complex128-aot-") as td:
        root = pathlib.Path(td)
        ir = root / "guest.ll"
        c = root / "harness.c"
        ir.write_text(compiled.stdout, encoding="utf-8")
        c.write_text(harness, encoding="utf-8")
        for optimization in ("-O0", "-O2"):
            exe = root / ("complex128-test-" + optimization[1:])
            native = subprocess.run(
                [args.clang, str(ir), str(c), "-std=c11", optimization,
                 "-o", str(exe)],
                capture_output=True, text=True, check=False, timeout=30)
            require(native.returncode == 0,
                    "Clang failed to link complex128 aggregate ABI "
                    + optimization + ": " + native.stderr)
            run = subprocess.run(
                [str(exe)], capture_output=True, text=True,
                check=False, timeout=10)
            require(run.returncode == 0,
                    f"complex128 native ABI check {optimization} failed "
                    f"with exit {run.returncode}")

    print("NATIVE_COMPLEX128_RETURN_CASES=9")
    print("NATIVE_COMPLEX128_RAW_COMPONENTS=18")
    print("NATIVE_COMPLEX128_IMAG_LITERAL_ROUNDING=JAVA_PARITY")
    print("NATIVE_COMPLEX128_OPT_LEVELS=2")

if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
        print("native complex128 AOT test failed:", exc, file=sys.stderr)
        sys.exit(1)
