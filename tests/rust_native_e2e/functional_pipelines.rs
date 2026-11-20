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


    // Cross-compiler supplemental edges: native LLVM IR, not host-simulated
    // pipelines. Each .atto source is the exact Git blob in atto-spec#121.
    for (name, expected_exit, sha) in [
        ("two-argument-61", 61,
         "4089c524db1d076593994ad65acb5ca7f90eea32"),
        ("preapplied-grouped-42", 42,
         "9a89ada18865fead0d92a7de75415957fbb5f67f"),
    ] {
        let source_file = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/spec-functional-pipelines-edge")
            .join(format!("{name}.atto"));
        let observed = std::process::Command::new("git")
            .arg("hash-object").arg(&source_file)
            .output().expect("hash canonical Atto source");
        assert!(observed.status.success(), "git hash-object failed for {name}");
        assert_eq!(String::from_utf8(observed.stdout).unwrap().trim(), sha,
            "canonical Atto edge source drift: {name}");
        let source = std::fs::read_to_string(&source_file)
            .expect("read canonical Atto edge source");
        h.positive(&format!("functional_pipe_edge_{name}"),
            &source, &[], expected_exit, false, None);
    }
}
