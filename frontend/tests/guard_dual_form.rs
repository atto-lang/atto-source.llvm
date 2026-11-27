//! Compiler-backed source parsing and Wasm code emission for both guard blocks.
//! Native executable parity requires the LLVM AOT pipeline/guest CI separately.
use atto_compiler::compile;

fn accepted(src: &str) {
    let wasm = compile(src).unwrap_or_else(|d| panic!("expected compiler acceptance: {d}\n{src}"));
    assert!(wasm.starts_with(b"\0asm\x01\0\0\0"));
}

fn rejected(src: &str, expected: &str) {
    let error = compile(src).expect_err("invalid guard must not emit a guest");
    assert_eq!(error.code, expected, "{src}: {error}");
}

#[test]
fn true_and_false_guards_compile_in_both_spellings() {
    for condition in ["true", "false"] {
        accepted(&format!("pub fnc main(): int {{ guard {condition} {{ return 0; }} return 42; }}"));
        accepted(&format!("pub fnc main(): int {{ guard {condition} else return 0; end return 42; }}"));
    }
}

#[test]
fn guard_keyword_form_supports_outer_if_and_nested_terminal_if() {
    accepted(r#"pub fnc main(): int {
        if true then
            guard true else
                if false then
                    return 1;
                else
                    return 2;
                fi
            end
        else
            return 3;
        fi
        return 42;
    }"#);
}

#[test]
fn guards_reject_nonterminal_failure_and_wrong_type() {
    rejected("pub fnc main(): int { guard true else val x = 1; end return 42; }", "E_GUARD");
    rejected("pub fnc main(): int { guard true { val x = 1; } return 42; }", "E_GUARD");
    rejected("pub fnc main(): int { guard 41 else return 0; end return 42; }", "E_TYPE");
}

#[test]
fn braces_cannot_be_followed_by_a_second_guard_else() {
    assert!(compile("pub fnc main(): int { guard false { return 0; } else return 1; end return 42; }").is_err());
    accepted("pub fnc main(): int { if true then guard true { return 7; } else return 9; fi return 42; }");
}

#[test]
fn missing_keyword_guard_end_fails_without_artifact() {
    assert!(compile("pub fnc main(): int { guard true else return 0; fi return 42; }").is_err());
}

#[test]
fn guard_failure_must_explicitly_return_not_just_terminate() {
    // A general terminating throw is not a lexical function return.
    for exit in [
        r#"throw Error("bad");"#,
        "break;",
        "continue;",
    ] {
        let braced = format!("pub fnc main(): int {{ guard false {{ {exit} }} return 42; }}");
        let keyword = format!("pub fnc main(): int {{ guard false else {exit} end return 42; }}");
        rejected(&braced, "E_GUARD");
        rejected(&keyword, "E_GUARD");
    }
}

#[test]
fn guard_failure_return_is_checked_even_if_condition_is_const_true() {
    rejected("pub fnc main(): int { guard true else break; end return 42; }", "E_GUARD");
    accepted("pub fnc main(): int { guard true else return 7; end return 42; }");
}

#[test]
fn earlier_nonreturn_exit_cannot_be_hidden_by_trailing_return() {
    for bad in [r#"throw Error("bad");"#, "break;", "continue;"] {
        for keyword in [false, true] {
            let source = if keyword {
                format!("pub fnc main(): int {{ guard false else {bad} return 0; end return 42; }}")
            } else {
                format!("pub fnc main(): int {{ guard false {{ {bad} return 0; }} return 42; }}")
            };
            rejected(&source, "E_GUARD");
        }
    }
}
