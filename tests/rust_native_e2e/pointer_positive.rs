use super::Harness;

// This is the restricted opt-in stack-promoted Pointer<int> profile, not general Pointer<T>.
pub fn run(h: &mut Harness) {
    h.positive("pointer_sum_142",
        "pub fnc main(): int {\n  let mut Pointer<int> p = Pointer.alloc<int>(4);\n  p.write_at(0, 42);\n  p.write_at(1, 100);\n  val int first = p.read_at(0);\n  val int second = p.read_at(1);\n  p.close();\n  return first + second;\n}\n",
        &["--experimental-pointer-int-aot"], 142, true, None);
    h.positive("pointer_inplace_6",
        "pub fnc main(): int {\n  let mut Pointer<int> p = Pointer.alloc<int>(4);\n  p.write_at(0, 2);\n  p.write_at(0, p.read_at(0) * 3);\n  val int first = p.read_at(0);\n  p.close();\n  return first;\n}\n",
        &["--experimental-pointer-int-aot"], 6, true, None);
    h.positive("pointer_last_index_255",
        "pub fnc main(): int {\n  let mut Pointer<int> p = Pointer.alloc<int>(16);\n  p.write_at(15, 255);\n  val int first = p.read_at(15);\n  p.close();\n  return first;\n}\n",
        &["--experimental-pointer-int-aot"], 255, true, None);
    h.positive("pointer_i64_upper_boundary",
        "pub fnc main(): int {\n  let mut Pointer<int> p = Pointer.alloc<int>(1);\n  p.write_at(0, 9223372036854775806 + 1);\n  val int first = p.read_at(0);\n  p.close();\n  return first - 9223372036854775807 + 42;\n}\n",
        &["--experimental-pointer-int-aot"], 42, true, None);
    h.positive("pointer_i64_lower_boundary",
        "pub fnc main(): int {\n  let mut Pointer<int> p = Pointer.alloc<int>(1);\n  p.write_at(0, -9223372036854775807 - 1);\n  val int first = p.read_at(0);\n  p.close();\n  return first - (-9223372036854775807 - 1) + 42;\n}\n",
        &["--experimental-pointer-int-aot"], 42, true, None);
    h.positive("pointer_drop_0",
        "pub fnc main(): int {\n  let mut Pointer<int> p = Pointer.alloc<int>(1);\n  p.close();\n  return 0;\n}\n",
        &["--experimental-pointer-int-aot"], 0, true, None);
    h.positive("pointer_singleton_methods_42",
        "pub fnc main(): int { let mut Pointer<int> p = Pointer.alloc<int>(1); p.write(21); val int first = p.read(); p.write(first * 2); val int answer = p.read(); p.close(); return answer; }",
        &["--experimental-pointer-int-aot"], 42, true, None);
    h.positive("pointer_singleton_mixed_indexed_42",
        "pub fnc main(): int { let mut Pointer<int> p = Pointer.alloc<int>(1); p.write_at(0, 21); val int first = p.read(); p.write(first * 2); val int answer = p.read_at(0); p.close(); return answer; }",
        &["--experimental-pointer-int-aot"], 42, true, None);

}
