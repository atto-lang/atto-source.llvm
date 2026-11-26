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
    h.negative("pointer_requires_opt_in",
        &src("  p.close();", "0", 4),
        &[], None);
    h.negative("pointer_rejects_unary_address",
        &src("  p.write_at(0, 1);\n  val int first = &p;\n  p.close();", "first", 4),
        &flag, Some("unsupported oreslang token"));
}
