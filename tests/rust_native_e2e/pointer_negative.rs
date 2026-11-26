use super::Harness;

// Fail before IR and leave the isolated compiler workspace with only input.atto.
fn src(steps: &str, outcome: &str, capacity: i32) -> String {
    format!(
        "pub fnc main(): int {{\n  let mut Pointer<int> p = Pointer.alloc<int>({});\n{}\n  return {};\n}}\n",
        capacity, steps, outcome
    )
}

pub fn run(h: &mut Harness) {
    let flag = ["--experimental-pointer-int-aot"];
    h.negative("pointer_uninitialized_read",
        &src("  val int first = p.read_at(0);\n  p.close();", "first", 4),
        &flag, Some("uninitialized"));
    h.negative("pointer_out_of_bounds",
        &src("  p.write_at(4, 1);\n  p.close();", "0", 4),
        &flag, Some("bounds"));
    h.negative("pointer_double_close",
        &src("  p.close();\n  p.close();", "0", 4),
        &flag, Some("double close"));
    h.negative("pointer_missing_close",
        &src("  p.write_at(0, 1);", "0", 4),
        &flag, Some("close exactly once"));
    h.negative("pointer_escaped_owner",
        &src("  val int first = p;\n  p.close();", "first", 4),
        &flag, Some("escape"));
    for (name, expr) in [
        ("pointer_add_overflow", "9223372036854775807 + 1"),
        ("pointer_sub_overflow", "(-9223372036854775807 - 1) - 1"),
        ("pointer_mul_overflow", "3037000500 * 3037000500"),
        ("pointer_mul_min_neg_one", "(-9223372036854775807 - 1) * -1"),
    ] {
        h.negative(name, &src("  p.close();", expr, 4), &flag, Some("overflow"));
    }
    h.negative("pointer_requires_opt_in",
        &src("  p.close();", "0", 4),
        &[], None);
    h.negative("pointer_rejects_unary_address",
        &src("  p.write_at(0, 1);\n  val int first = &p;\n  p.close();", "first", 4),
        &flag, Some("unsupported oreslang token"));
    h.negative("pointer_singleton_read_multi",
        &src("  p.write_at(0, 42);\n  val int first = p.read();\n  p.close();", "first", 2),
        &flag, Some("read requires singleton allocation"));
    h.negative("pointer_singleton_write_multi",
        &src("  p.write(42);\n  p.close();", "0", 2),
        &flag, Some("write requires singleton allocation"));
    h.negative("pointer_singleton_read_uninitialized",
        &src("  val int first = p.read();\n  p.close();", "first", 1),
        &flag, Some("uninitialized"));
    h.negative("pointer_singleton_read_closed",
        &src("  p.write(42);\n  p.close();\n  val int first = p.read();", "first", 1),
        &flag, Some("read after close"));
    h.negative("pointer_singleton_write_closed",
        &src("  p.close();\n  p.write(42);", "0", 1),
        &flag, Some("write after close"));
    h.negative("pointer_singleton_read_wrong_arity",
        &src("  p.write(42);\n  val int first = p.read(0);\n  p.close();", "first", 1),
        &flag, Some("expected"));
    h.negative("pointer_singleton_write_wrong_arity",
        &src("  p.write(0, 42);\n  p.close();", "0", 1),
        &flag, Some("expected"));

    h.negative("pointer_metadata_after_close",
        &src("  p.close();\n  val int first = p.len();", "first", 1),
        &flag, Some("read after close"));
    h.negative("pointer_size_wrong_arity",
        &src("  val int first = p.size_bytes(0);\n  p.close();", "first", 1),
        &flag, Some("expected"));
    h.negative("pointer_initializer_self_reference",
        "pub fnc main(): int { let mut Pointer<int> p = Pointer.alloc_value<int>(p.len()); p.close(); return 0; }",
        &flag, Some("owner unavailable during initializer"));
    h.negative("pointer_initializer_overflow",
        "pub fnc main(): int { let mut Pointer<int> p = Pointer.alloc_value<int>(9223372036854775807 + 1); p.close(); return 0; }",
        &flag, Some("overflow"));

    h.negative("pointer_fill_closed", &src("  p.close();\n  p.fill(42);", "0", 2), &flag, Some("fill after close"));
    h.negative("pointer_fill_uninitialized_source", &src("  p.fill(p.read_at(0));\n  p.close();", "0", 2), &flag, Some("uninitialized"));
    h.negative("pointer_fill_overflow", &src("  p.fill(9223372036854775807 + 1);\n  p.close();", "0", 2), &flag, Some("overflow"));
    h.negative("pointer_swap_uninitialized", &src("  p.write_at(0, 42);\n  p.swap_at(0, 1);\n  p.close();", "0", 2), &flag, Some("initialized"));
    h.negative("pointer_swap_out_of_bounds", &src("  p.fill(42);\n  p.swap_at(0, 2);\n  p.close();", "0", 2), &flag, Some("bounds"));
    h.negative("pointer_swap_closed", &src("  p.fill(42);\n  p.close();\n  p.swap_at(0, 1);", "0", 2), &flag, Some("swap after close"));

    let helper = "fnc inspect(Pointer<int> q): int { return q.read(); }";
    for (name, body, diagnostic) in [
        ("borrow_uninitialized", "let mut Pointer<int> p = Pointer.alloc<int>(1); val int a = inspect(p); p.close(); return a;", "uninitialized"),
        ("borrow_closed", "let mut Pointer<int> p = Pointer.alloc_value<int>(42); p.close(); return inspect(p);", "unavailable owner"),
        ("borrow_wrong_owner", "let mut Pointer<int> p = Pointer.alloc_value<int>(42); val int a = inspect(other); p.close(); return a;", "expected"),
        ("borrow_multi", "let mut Pointer<int> p = Pointer.alloc<int>(2); p.fill(42); val int a = inspect(p); p.close(); return a;", "singleton"),
        ("borrow_gap", "let mut Pointer<int> p = Pointer.alloc_value<int>(42); val int a = inspect(borrow ! p); p.close(); return a;", "adjacent"),
        ("borrow_mut_not_admitted", "let mut Pointer<int> p = Pointer.alloc_value<int>(42); val int a = inspect(borrow! mut p); p.close(); return a;", "expected"),
    ] {
        let source = format!("{helper} pub fnc main(): int {{ {body} }}");
        h.negative(name, &source, &flag, Some(diagnostic));
    }
    h.negative("borrow_helper_close",
        "fnc inspect(Pointer<int> q): int { q.close(); return 42; } pub fnc main(): int { let mut Pointer<int> p = Pointer.alloc_value<int>(42); val int a = inspect(p); p.close(); return a; }", &flag, Some("expected"));

}
