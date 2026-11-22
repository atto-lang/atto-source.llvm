use super::Harness;

// Every case passes through the real normal compiler. Invalid typed foreign
// declarations and provenance-grant misuse must leave NO LLVM IR or artifacts.
const GRANT: &str = "--ffi-grant=atto.math.i64.v1";
const DECL: &str = "extern fnc atto_add_i64(int a, int b): int using ffi;\n";
fn src(declaration: &str, effects: &str, left: &str, right: &str) -> String {
    format!("{}pub fnc main(): int {} {{ return atto_add_i64({}, {}); }}\n",
            declaration, effects, left, right)
}
fn normal() -> String {
    src(DECL, "using ffi", "20", "22")
}

pub fn run(h: &mut Harness) {
    let ordinary = normal();
    h.negative("normal_ffi_reject_ungranted", &ordinary, &[], None);
    h.negative("normal_ffi_reject_wrong_symbol",
        &src(&DECL.replace("atto_add_i64", "printf"), "using ffi", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_bool_return",
        &src(&DECL.replace("): int", "): bool"), "using ffi", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_bool_parameter",
        &src(&DECL.replace("int a", "bool a"), "using ffi", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_borrow_parameter",
        &src(&DECL.replace("int a", "borrow int a"), "using ffi", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_missing_declaration_effect",
        &src(&DECL.replace(" using ffi", ""), "using ffi", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_missing_main_effect",
        &src(DECL, "", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_nested_foreign_calls",
        &ordinary.replace("atto_add_i64(20, 22)", "atto_add_i64(atto_add_i64(1,2),22)"),
        &[GRANT], None);
    h.negative("normal_ffi_reject_source_override",
        &src("fnc atto_add_i64(int a, int b): int { return a+b; }\n",
             "using ffi", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_dynamic_library",
        &src(&DECL.replace("extern fnc", "extern library \"evil.so\" fnc"),
             "using ffi", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_pointer_parameter",
        &src(&DECL.replace("int a", "Pointer<int> a"), "using ffi", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_oversized_literal",
        &src(DECL, "using ffi", "9223372036854775808", "0"), &[GRANT], None);
    h.negative("normal_ffi_reject_signed_overflow",
        &src(DECL, "using ffi", "9223372036854775807", "1"), &[GRANT], None);
    h.negative("normal_ffi_reject_signed_underflow",
        &src(DECL, "using ffi", "-9223372036854775807", "-2"), &[GRANT], None);
    h.negative("normal_ffi_reject_unknown_effect",
        &src(DECL, "using foo", "20", "22"), &[GRANT], None);
    h.negative("normal_ffi_reject_async_foreign",
        &src(&DECL.replace("extern fnc", "extern async fnc"),
             "using ffi", "20", "22"), &[GRANT], None);
    println!("ATTO_RUST_NORMAL_FFI_FAIL_CLOSED_REJECTIONS=16");
}
