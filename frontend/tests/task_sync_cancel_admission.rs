//! Task parity admission against Rust canonical target. No guest runtime is implied.
use atto_compiler::{compile, parser::is_reserved};

fn reject_unlowered(source: &str) {
    let d = compile(source).expect_err("unverified task lowering must not emit a Wasm guest");
    assert_eq!(d.code, "E_UNSUPPORTED", "{d}");
}

#[test]
fn sync_is_whole_word_reserved_not_a_user_variable() {
    assert!(is_reserved("sync"));
    assert!(!is_reserved("synchronize"));
    assert!(!is_reserved("sync_data"));
    let d = compile("pub fnc main(): int { val sync = 42; return sync; }")
        .expect_err("sync must not be a local");
    assert_eq!(d.code, "E_RESERVED", "{d}");
}

#[test]
fn all_task_spellings_reject_without_structured_scope_runtime() {
    for source in [
        "pub fnc main(): int { val x = task<int> { return 42; }; return 0; }",
        "pub fnc main(): int { val x = task { return 42; }; return 0; }",
        "pub fnc main(): int { val x = sync task<int> { return 42; }; return 0; }",
        "pub fnc main(): int { val x = await task<int> { return 42; }; return 0; }",
        "pub fnc main(): int { do task<int> { return 42; }; return 0; }",
        "pub fnc main(): int { do sync task<int> { return 42; }; return 0; }",
    ] {
        reject_unlowered(source);
    }
}
