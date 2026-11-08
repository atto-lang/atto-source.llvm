#!/usr/bin/env python3
"""Negative tests for the scalar AOT IR oracle (no LLVM SDK needed)."""

from native_rt_scalar_step_aot import (verify_closed_guest_ir,
                                      parse_clang_major,
                                      verify_matching_llvm_clang)
from types import SimpleNamespace
from unittest.mock import patch


IR = """declare void @llvm.trap()
define void @oreslang_rt_scalar_frame_init_v1(ptr %x, i64 %fuel) { ret void }
define i32 @oreslang_rt_scalar_frame_step_v1(ptr %x, i64 %fuel) {
entry:
  call void @llvm.trap()
  ret i32 0
}
define void @oreslang_rt_scalar_frame_cancel_v1(ptr %x) { ret void }
define i64 @oreslang_rt_scalar_frame_result_v1(ptr %x) { ret i64 42 }
"""


def rejected(source):
    try:
        verify_closed_guest_ir(source)
    except AssertionError:
        return True
    return False


def require_rejected(source):
    if not rejected(source):
        raise AssertionError("insecure LLVM IR was unexpectedly admitted")


def main():
    verify_closed_guest_ir(IR)
    require_rejected(IR.replace("declare void @llvm.trap()",
                               "declare void @untrusted_ffi()"))
    require_rejected(IR.replace("define void @oreslang_rt_scalar_frame_cancel_v1",
                               "define void @renamed_cancel"))
    require_rejected(IR + "\ndefine void @unverified_helper() { ret void }\n")
    require_rejected(IR + "\n  call void @oreslang_rt_scalar_frame_step_v1()\n")
    require_rejected(IR + "\n  call void %untrusted_function_pointer()\n")
    require_rejected(IR + "\n  call void asm sideeffect \"\", \"\"()\n")
    require_rejected(IR + "\n  invoke void @llvm.trap() to label %ok unwind label %bad\n")
    require_rejected(IR + "\n  callbr void @llvm.trap() to label %ok []\n")
    require_rejected(IR.replace("declare void @llvm.trap()", ""))
    # ABI-incompatible symbols must be rejected, even when names are intact.
    require_rejected(IR.replace("define i32 @oreslang_rt_scalar_frame_step_v1",
                                "define i64 @oreslang_rt_scalar_frame_step_v1"))
    require_rejected(IR.replace("ptr %x, i64 %fuel", "ptr %x", 1))
    require_rejected(IR.replace("ptr %x, i64 %fuel", "i64 %x, i64 %fuel", 1))
    require_rejected(IR.replace("ptr %x, i64 %fuel", "i64* %x, i64 %fuel", 1))
    require_rejected(IR.replace("ptr %x)", "ptr %x, ...)", 1))
    require_rejected(IR.replace("define void @oreslang_rt_scalar_frame_cancel_v1",
                                "define internal void @oreslang_rt_scalar_frame_cancel_v1"))
    require_rejected(IR.replace("define void @oreslang_rt_scalar_frame_init_v1",
                                "define void @oreslang_rt_scalar_frame_init_v1") +
                     "\ndefine i32 @oreslang_rt_scalar_frame_step_v1(ptr %x) { ret i32 0 }\n")
    # Typed-pointer LLVM 14-style output remains accepted with the correct
    # struct frame pointer; do not accidentally re-allow untyped i64*.
    verify_closed_guest_ir(IR.replace("ptr %x", "%oreslang.rt.scalar.frame.v1* %x"))
    # AOT Clang and linked LLVM SDK must be the same major. Mocking only
    # the version probe ensures this negative test is portable and executes
    # without installed LLVM/Clang developer libraries.
    for sample, expected in [("17.0.0\n", 17), ("19", 19), ("22.1.7", 22)]:
        if parse_clang_major(sample) != expected:
            raise AssertionError("clang version major parsed incorrectly")
    for sample in ["", "clang version 19.0", "v19", "19.1 extra", "NaN"]:
        try:
            parse_clang_major(sample)
        except ValueError:
            continue
        raise AssertionError("bad clang version output unexpectedly accepted")
    with patch("native_rt_scalar_step_aot.run", return_value=SimpleNamespace(
            returncode=0, stderr="", stdout="17.0.0\n")):
        if verify_matching_llvm_clang("clang", 17) != 17:
            raise AssertionError("matching Clang version denied")
        try:
            verify_matching_llvm_clang("clang", 19)
        except RuntimeError:
            pass
        else:
            raise AssertionError("mismatched LLVM SDK/Clang unexpectedly accepted")
    with patch("native_rt_scalar_step_aot.run", return_value=SimpleNamespace(
            returncode=1, stderr="not found", stdout="")):
        try:
            verify_matching_llvm_clang("missing-clang", 19)
        except RuntimeError:
            pass
        else:
            raise AssertionError("failed compiler version probe unexpectedly accepted")
    print("SCALAR_GUEST_IR_SURFACE_SCAN_PASS")


if __name__ == "__main__":
    main()
