//! Exact portable guarded-return corpus, plus negative type/control-flow gates.
//! These are compiler tests; passing them does not prove native LLVM AOT execution.

use atto_compiler::compile;

#[test]
fn exact_return_match_block_and_expression_arms_compile() {
    let cases = [
        include_str!("fixtures/pass/match-guard-block-return.atto"),
        include_str!("fixtures/pass/match-guard-block-caret.atto"),
    ];
    for (i, source) in cases.iter().enumerate() {
        let guest = compile(source).unwrap_or_else(|err| {
            panic!("guarded return match case {i} rejected: {err}")
        });
        assert!(guest.starts_with(b"\0asm"), "case {i}: not a Wasm guest");
        assert!(guest.len() > 8, "case {i}: truncated Wasm guest");
    }
}

#[test]
fn guarded_return_match_negative_contracts_fail_without_guest_artifacts() {
    let cases = [
        (include_str!("fixtures/fail/match-guard-no-fallback.atto"), "E_MATCH"),
        (include_str!("fixtures/fail/match-guard-block-fallthrough.atto"), "E_RETURN"),
    ];
    for (i, (source, code)) in cases.iter().enumerate() {
        match compile(source) {
            Err(diagnostic) => assert_eq!(diagnostic.code, *code, "case {i}: {diagnostic}"),
            Ok(_) => panic!("case {i} emitted a guest artifact for an invalid return match"),
        }
    }
}

#[test]
fn caret_is_a_return_alias_not_an_implicit_infix_operator() {
    // LLVM frontend's XOR lowering is not admitted by this profile yet.
    let valid = "pub fnc main(): int { ^ 7; }";
    assert!(compile(valid).is_ok(), "statement caret return should compile");
    let invalid = "pub fnc main(): int { return 2 ^ 3; }";
    assert!(compile(invalid).is_err(), "infix caret must fail closed, not be miscompiled as return");
}

#[test]
fn guarded_return_match_rejects_missing_value_arm_comma() {
    let invalid = "pub fnc main(): int { ^ match 7 over on 0 -> 0 on _ -> 1 end; }";
    let diagnostic = compile(invalid).expect_err("missing separator emitted guest artifact");
    assert_eq!(diagnostic.code, "E_SYNTAX");
}

#[test]
fn guarded_return_match_rejects_nonboolean_guard() {
    let invalid = "pub fnc main(): int { return match 2 over on n when n + 1 -> 2, on _ -> 0 end; }";
    let diagnostic = compile(invalid).expect_err("nonbool guard emitted guest artifact");
    assert_eq!(diagnostic.code, "E_TYPE");
}
