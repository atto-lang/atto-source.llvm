use super::Harness;

// Restricted M0 proof: one fixed static i64 C symbol, NOT actor FFI or arbitrary libraries.
const MODE: &str = "--experimental-static-ffi-i64-aot";
const GRANT: &str = "--ffi-grant=atto.math.i64.v1";
const C_SHIM: &str = "#include <stdint.h>\n_Static_assert(sizeof(int64_t)==8, \"i64 must be 8 bytes\");\nint64_t atto_add_i64(int64_t a, int64_t b) { return a+b; }\n";

fn source(a: i32, b: i32) -> String {
    format!(
        "define ffi actor NativeMath as\n  extern suspends fnc add_i64(int a, int b): int;\nend\npub suspends fnc main(): int using ffi, suspend {{\n  return await NativeMath.add_i64({}, {});\n}}\n",
        a, b
    )
}

pub fn run(h: &mut Harness) {
    h.positive("fixed_static_ffi_42",
        &source(20, 22), &[MODE, GRANT], 42, true, Some(C_SHIM));
    h.positive("fixed_static_ffi_3",
        &source(1, 2), &[MODE, GRANT], 3, true, Some(C_SHIM));
    h.negative("ffi_no_grant",
        &source(20, 22), &[MODE], None);
    h.negative("ffi_no_mode",
        &source(20, 22), &[GRANT], None);
    h.negative("ffi_rejects_foreign_alias",
        &source(20, 22).replace("add_i64(int a, int b)", "printf(int a, int b)"),
        &[MODE, GRANT], None);
    h.negative("ffi_rejects_untrusted_actor",
        &source(20, 22).replace("define ffi actor", "define untrusted ffi actor"),
        &[MODE, GRANT], None);
    h.negative("ffi_rejects_overflowed_exit",
        &source(255, 1), &[MODE, GRANT], None);
}
