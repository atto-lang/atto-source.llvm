use super::Harness;

pub fn run(h: &mut Harness) {
    // Real Atto source -> LLVM IR -> Clang -> native guest, not a Rust model.
    h.positive("functional_pipe_blocks",
        r#"pub fnc double(int x): int { return x * 2; }
pub fnc increment(int x): int { return x + 1; }
pub fnc main(): int {
    val a = 20 |> double |> increment;
    val b = double <| 21;
    return a + b;
}"#, &[], 83, false, None);

    h.positive("functional_pipe_inline_arrow",
        r#"pub fnc double(int x): int -> x * 2;
pub fnc increment(int x): int -> x + 1;
pub fnc main(): int {
    val a = 20 |> double |> increment;
    val b = double <| 21;
    return a + b;
}"#, &[], 83, false, None);

    h.positive("functional_pipe_nested_right_associative",
        r#"fnc double(int x): int -> x * 2;
fnc increment(int x): int -> x + 1;
pub fnc main(): int -> double <| increment <| 20;"#,
        &[], 42, false, None);

    h.positive("functional_pipe_grouped_mixed_direction",
        r#"fnc double(int x): int -> x * 2;
fnc increment(int x): int -> x + 1;
pub fnc main(): int -> double <| (20 |> increment);"#,
        &[], 42, false, None);

    // Do not emit LLVM IR for invalid type, malformed expression or
    // unparenthesized mixed direction: no source-to-host fallback.
    h.negative("functional_pipe_wrong_return",
        r#"fnc double(int x): int -> true;
pub fnc main(): int { return double(21); }"#, &[], None);
    h.negative("functional_pipe_missing_semicolon",
        r#"fnc double(int x): int -> x * 2
pub fnc main(): int { return double(21); }"#, &[], None);
    h.negative("functional_pipe_wrong_callee",
        r#"pub fnc main(): int { return 21 <| 42; }"#, &[], None);
    h.negative("functional_pipe_ungrouped_mixed",
        r#"fnc double(int x): int { return x * 2; }
pub fnc main(): int { return 20 |> double <| 21; }"#, &[], None);
    h.negative("functional_pipe_ungrouped_reverse_then_forward",
        r#"fnc double(int x): int -> x * 2;
fnc increment(int x): int -> x + 1;
pub fnc main(): int -> double <| 20 |> increment;"#, &[], None);
}
