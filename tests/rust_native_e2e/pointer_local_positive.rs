use super::Harness;

// These are actual compiled Atto source -> LLVM IR -> Clang -> native
// executable guest tests, with an independent ASan/UBSan guest run.
pub fn run(h: &mut Harness) {
    let flags = ["--experimental-pointer-int-aot"];
    h.positive("slot_rebind_42", "pub fnc main(): int { let a: int = 40; let b: int = 2; let mut RawPointer<int> p = RawPointer.empty<int>(); ref! p to a; val first = deref! p; unref! p; ref! p to b; val second = deref! p; unref! p; return first + second; }", &flags, 42, true, None);
    h.positive("slot_empty_42", "pub fnc main(): int { let mut RawPointer<int> p = RawPointer.empty<int>(); return 42; }", &flags, 42, true, None);
    h.positive("slot_survivor_42", "pub fnc main(): int { let a: int = 42; let mut RawPointer<int> p = RawPointer.empty<int>(); ref! p to a; unref! p; return a; }", &flags, 42, true, None);
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
