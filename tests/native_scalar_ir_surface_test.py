#!/usr/bin/env python3
"""Negative tests for the scalar AOT IR oracle (no LLVM SDK needed)."""

from native_rt_scalar_step_aot import verify_closed_guest_ir


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


def main():
    verify_closed_guest_ir(IR)
    assert rejected(IR.replace("declare void @llvm.trap()",
                               "declare void @untrusted_ffi()"))
    assert rejected(IR.replace("define void @oreslang_rt_scalar_frame_cancel_v1",
                               "define void @renamed_cancel"))
    assert rejected(IR + "\ndefine void @unverified_helper() { ret void }\n")
    assert rejected(IR + "\n  call void @oreslang_rt_scalar_frame_step_v1()\n")
    assert rejected(IR + "\n  call void %untrusted_function_pointer()\n")
    assert rejected(IR + "\n  call void asm sideeffect \"\", \"\"()\n")
    assert rejected(IR + "\n  invoke void @llvm.trap() to label %ok unwind label %bad\n")
    assert rejected(IR + "\n  callbr void @llvm.trap() to label %ok []\n")
    assert rejected(IR.replace("declare void @llvm.trap()", ""))
    print("SCALAR_GUEST_IR_SURFACE_SCAN_PASS")


if __name__ == "__main__":
    main()
