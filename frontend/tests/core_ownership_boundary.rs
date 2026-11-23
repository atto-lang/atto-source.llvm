//! Real compiler AST/checker coverage for core ownership invariants.
//! These run the frontend's native Rust checker and Wasm emission;
//! they do not constitute a native LLVM AOT or cross-compiler receipt.
use atto_compiler::{compile, check, lexer, parser::Parser};

const USE_AFTER_MOVE: &str = include_str!("fixtures/borrow_move_after_transfer.atto");
const OWNER_AFTER_MOVE: &str = include_str!("fixtures/borrow_move_owner_ok.atto");

#[test]
fn class_owner_use_after_transfer_is_rejected_before_artifact_emission() {
    let tokens = lexer::lex(USE_AFTER_MOVE).expect("source lexes");
    let ast = Parser::new(tokens).parse_program().expect("source parses");
    let error = check::check(&ast).expect_err("moved class owner is not Copy");
    assert_eq!(error.code, "E_MOVED", "{error}");
    let error = compile(USE_AFTER_MOVE).expect_err("no Wasm artifact after move violation");
    assert_eq!(error.code, "E_MOVED", "{error}");
}

#[test]
fn only_the_new_class_owner_can_access_the_transferred_value() {
    let tokens = lexer::lex(OWNER_AFTER_MOVE).expect("source lexes");
    let ast = Parser::new(tokens).parse_program().expect("source parses");
    check::check(&ast).expect("transferred owner retains exclusive access");
    let wasm = compile(OWNER_AFTER_MOVE).expect("valid ownership emits Wasm");
    assert!(wasm.starts_with(b"\0asm"), "must produce real Wasm, not host shim");
}

#[test]
fn scalar_values_remain_copyable_through_core_checks() {
    let source = "pub fnc main(): int { val int x = 21; val int y = x; return x + y; }";
    let wasm = compile(source).expect("scalar copies require no borrow");
    assert!(wasm.starts_with(b"\0asm"));
}
