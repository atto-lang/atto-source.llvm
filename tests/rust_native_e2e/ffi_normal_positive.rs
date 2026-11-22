use super::Harness;

// Real normal Atto parser/typed HIR -> LLVM IR -> C i64 ABI -> native execution.
// Unlike the old restricted M0 parser, normal source uses a declared extern fnc.
const GRANT: &str = "--ffi-grant=atto.math.i64.v1";
const C_SHIM: &str = "#include <stdint.h>\n_Static_assert(sizeof(int64_t) == 8, \"i64 ABI width\");\nint64_t atto_add_i64(int64_t a, int64_t b) { return a + b; }\n";
fn source(a: &str, b: &str) -> String {
    format!("extern fnc atto_add_i64(int a, int b): int using ffi;\n\
             pub fnc main(): int using ffi {{ return atto_add_i64({}, {}); }}\n", a, b)
}

pub fn run(h: &mut Harness) {
    // Every fixture invokes the actual C++ Atto compiler and the Clang linker;
    // sanitizer=true executes independently compiled Release + ASan/UBSan.
    for (name, a, b, exit) in [
        ("normal_ffi_42", "20", "22", 42),
        ("normal_ffi_zero", "0", "0", 0),
        ("normal_ffi_i64_max_plus_negative", "9223372036854775807", "-9223372036854775765", 42),
        ("normal_ffi_i64_max_cancels", "9223372036854775807", "-9223372036854775807", 0),
        ("normal_ffi_negative_to_positive", "-17", "59", 42),
    ] {
        h.positive(name, &source(a, b), &[GRANT], exit, true, Some(C_SHIM));
    }
    println!("ATTO_RUST_NORMAL_FFI_SIGNED_I64_GUESTS=5");
}
