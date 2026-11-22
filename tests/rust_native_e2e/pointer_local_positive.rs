use super::Harness;

// These are actual compiled Atto source -> LLVM IR -> Clang -> native
// executable guest tests, with an independent ASan/UBSan guest run.
pub fn run(h: &mut Harness) {
    let flags = ["--experimental-pointer-int-aot"];
    let rt = "pub fnc main(): int {\n  let value: int = 42;\n  val Pointer<int> ptr = rt ref value;\n  val answer = rt deref ptr;\n  rt unref ptr;\n  return answer;\n}\n";
    let mixed = "pub fnc main(): int {\n  let value: int = 42;\n  val Pointer<int> ptr = rt ref value;\n  val answer = deref! ptr;\n  unref! ptr;\n  return answer;\n}\n";
    h.positive("pointer_local_exact_rt_42", rt, &flags, 42, true, None);
    h.positive("pointer_local_exact_mixed_42", mixed, &flags, 42, true, None);
    h.positive("pointer_local_compact_bang_42",
        "pub fnc main(): int { let value: int = 42; val Pointer<int> ptr = ref!value; val answer = deref!ptr; unref!ptr; return answer; }",
        &flags, 42, true, None);
    h.positive("pointer_local_named_13",
        "pub fnc main(): int { let score: int = 13; val Pointer<int> lease = ref! score; val observed = rt deref lease; rt unref lease; return observed; }",
        &flags, 13, true, None);
}
