//! Narrow guest-checked inline task admission. Independently scheduled tasks
//! still fail closed until TaskScope, Future cancellation and cleanup land.
use atto_compiler::{compile, parser::is_reserved};

fn reject_unlowered(source: &str, expected: &str) {
    let d = compile(source).expect_err("unverified task lowering must not emit a guest");
    assert_eq!(d.code, expected, "{d}");
}

#[test]
fn sync_is_whole_word_reserved_not_a_user_variable() {
    assert!(is_reserved("sync"));
    assert!(!is_reserved("synchronize"));
    assert!(!is_reserved("sync_data"));
    reject_unlowered("pub fnc main(): int { val sync = 42; return sync; }", "E_RESERVED");
}

#[test]
fn pure_sync_task_compiles_to_real_guest_bytes() {
    for source in [
        "pub fnc main(): int { val int x = sync task<int> { return 42; }; return x; }",
        "pub fnc main(): int { val int x = sync task { val int y = 5; return y + 7; }; return x; }",
        "pub fnc main(): int { do sync task<void> {}; return 9; }",
        "pub fnc main(): int { do sync task {}; return 9; }",
        "pub fnc main(): int { do sync task<void> { val int n = 5; }; return 9; }",
        "pub fnc main(): int { do sync task<void> { return; }; return 9; }",
        "pub fnc main(): int { do sync /* legal comment */ task<void> { }; return 9; }",
        "pub fnc main(): int { do sync task { return; }; return 9; }",
        "pub fnc main(): int { let int n = 1; do sync task<void> { n = n + 5; do sync task { n = n + 7; }; }; return n; }",
        "pub fnc main(): int { let int n = 7; do sync task { val int n = 12; }; return n; }",
        "pub fnc main(): int { sync task {}; return 9; }",
        "pub fnc main(): int { sync task<void> { return; }; return 9; }",
        "pub fnc main(): int { sync /* lexed comment */ task {}; return 9; }",
        "pub fnc main(): int { val bool b = sync task<bool> { return true; }; if b then return 5; else return 4; fi }",
        "pub fnc main(): int { val int x = sync task<int> { return sync task<int> { return 4; } + 3; }; return x; }",
    ] {
        let bytes = compile(source).unwrap_or_else(|e| panic!("inline task did not compile: {e}"));
        assert!(bytes.starts_with(b"\0asm"), "missing real Wasm guest artifact");
    }
}

#[test]
fn task_spawning_and_unverified_inline_effects_still_fail_closed() {
    for source in [
        "pub fnc main(): int { val x = task<int> { return 42; }; return 0; }",
        "pub fnc main(): int { val x = task { return 42; }; return 0; }",
        "pub fnc main(): int { val x = await task<int> { return 42; }; return 0; }",
        "pub fnc main(): int { do task<int> { return 42; }; return 0; }",
        "pub fnc main(): int { val x = sync task<int> { return 1; return 2; }; return x; }",
        "pub fnc main(): int { val x = sync task<int> { return compute(); }; return x; }",
        "pub fnc main(): int { do sync task { defer clean(); }; return 1; }",
        "pub fnc main(): int { val x = sync task<int> { return await future; }; return x; }",
    ] {
        reject_unlowered(source, "E_UNSUPPORTED");
    }
    // The do-inline form is void-only even if it could discard a result.
    for source in [
        "pub fnc main(): int { do sync task<int> { return 5; }; return 9; }",
        "pub fnc main(): int { do sync task<int> {}; return 9; }",
        "pub fnc main(): int { do sync task<bool> { return; }; return 9; }",
        "pub fnc main(): int { do sync task<int> { return await future; }; return 9; }",
        "pub fnc main(): int { do sync task<Result<int,string>> { return await future; }; return 9; }",
        "pub fnc main(): int { do sync task { return 5; }; return 9; }",
        "pub fnc main(): int { do sync task<void> { return 5; }; return 9; }",
        "pub fnc main(): int { do sync task { return sync task<int> { return 3; }; }; return 9; }",
    ] {
        reject_unlowered(source, "E_TASK_RETURN_TYPE");
    }
    reject_unlowered(
        "pub fnc main(): int { sync other {}; return 9; }",
        "E_UNSUPPORTED",
    );
    reject_unlowered(
        "pub fnc main(): int { sync task<int> { return; }; return 9; }",
        "E_TASK_RETURN_TYPE",
    );
    reject_unlowered(
        "pub fnc main(): int { val x = sync task<int> {}; return x; }",
        "E_TASK_RETURN_TYPE",
    );
    reject_unlowered(
        "pub fnc main(): int { val x = sync task<bool> { return 1; }; return 0; }",
        "E_TASK_RETURN_TYPE",
    );
}
