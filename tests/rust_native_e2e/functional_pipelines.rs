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

    // The remaining five spec edge files are checked against their exact
    // Git source blobs. LLVM/native must observe i64::MIN through the
    // compiler-emitted full-width guest function, not OS exit truncation.
    for (name, sha, kind) in [
        ("signed-i64-min", "bbc22347d7ccb0a5be1a7c2193f11c5f2f4805cc", "full"),
        ("checked-overflow", "59bb890b54addc1659065ab55cd565771523bfce", "overflow"),
        ("reject-i64-positive-overflow", "2e584ee01ee74a3a31e282cf0116957f9ff95bd1", "range"),
        ("reject-i64-negative-underflow", "8b63b45e406db3e19aa793e0f7a627914623b9e0", "range"),
        ("reject-reserved-unsafe", "b159def3fa26ca6023395e1c546b95c1b929fd14", "reserved"),
    ] {
        let file=std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/spec-functional-pipelines-edge")
            .join(format!("{name}.atto"));
        let sha_out=std::process::Command::new("git")
            .arg("hash-object").arg(&file).output()
            .expect("git hash-object canonical signed i64 fixture");
        assert!(sha_out.status.success(), "{name}: source SHA check failed");
        assert_eq!(String::from_utf8(sha_out.stdout).unwrap().trim(), sha,
            "{name}: source drift from atto-spec");
        let source=std::fs::read_to_string(&file)
            .expect("read independent Atto spec edge file");
        match kind {
            "full" => h.positive_full_i64(
                "functional_edge_signed_min", &source, "-9223372036854775808"),
            "overflow" => h.runtime_checked_overflow(
                "functional_edge_checked_overflow", &source),
            "range" => h.negative(
                &format!("functional_edge_{name}"), &source, &[], Some("E_INTEGER_RANGE")),
            "reserved" => h.negative(
                "functional_edge_reserved_authority", &source, &[], None),
            _ => unreachable!("all edge cases explicitly classified"),
        }
    }

    // Never interpret reserved actor, task and effect keywords as scalar
    // function names merely because they appear near a functional |> pipe.
    for keyword in ["unsafe", "sync", "task", "pipe", "rpipe"] {
        let source = format!(
            "fnc {keyword}(int x): int -> x;\npub fnc main(): int -> 42 |> {keyword};"
        );
        h.negative(&format!("functional_edge_reserved_{keyword}"),
            &source, &[], None);
    }
}
