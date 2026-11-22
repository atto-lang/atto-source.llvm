//! Independent LLVM-repo Rust/Wasm frontend proof for nominal ADT alias
//! spellings. This is NOT a native C++ LLVM process-stdio guest receipt.
use atto_compiler::compile;

fn compile_case(source:&str)->Vec<u8> {
    let wasm=compile(source).unwrap_or_else(|e|panic!("compiler: {e}\n{source}"));
    assert!(wasm.starts_with(b"\0asm\x01\0\0\0"));
    wasm
}
const RESULT_OLD:&str=r#"
fnc checked(): Result<int,string> { return Ok(42); }
pub fnc main(): int {
  return match checked() over
    on Ok(value) -> value,
    on Err(error) -> 0
  end;
}
"#;
const RESULT_NEW:&str=r#"
fnc checked(): Result<int,string> { return Ok(42); }
pub fnc main(): int {
  return match checked() over
    on is Ok<value> -> value,
    on is Err<error> -> 0
  end;
}
"#;
const OPTION_OLD:&str=r#"
fnc selected(): Option<int> { return Some(42); }
pub fnc main(): int {
  return match selected() over
    on Some(item) -> item,
    on None -> 0
  end;
}
"#;
const OPTION_NEW:&str=r#"
fnc selected(): Option<int> { return Some(42); }
pub fnc main(): int {
  return match selected() over
    on is Some<item> -> item,
    on is None -> 0
  end;
}
"#;
#[test]
fn old_and_is_adt_variants_emit_identical_wasm() {
    assert_eq!(compile_case(RESULT_OLD),compile_case(RESULT_NEW));
    assert_eq!(compile_case(OPTION_OLD),compile_case(OPTION_NEW));
}
#[test]
fn is_ok_wildcard_does_not_read_or_bind_result_payload() {
    let old=RESULT_OLD.replace("Ok(value) -> value","Ok(_) -> 42");
    let new=RESULT_NEW.replace("Ok<value> -> value","Ok<_> -> 42");
    assert_eq!(compile_case(&old),compile_case(&new));
}
#[test]
fn wrong_adt_family_and_unsafe_extractors_fail_closed() {
    for (src,expected) in [
        (RESULT_NEW.replace("is Ok<value>","is Some<value>"),"E_TYPE"),
        (RESULT_NEW.replace("is Ok<value>","is None<value>"),"E_ARITY"),
        (RESULT_NEW.replace("is Ok<value>","is ArbitraryExtractor<value>"),"E_PATTERN"),
        (RESULT_NEW.replace("is Ok<value>","is Ok<Some<value>>"),"E_SYNTAX"),
        (RESULT_NEW.replace("is Ok<value>","is Ok<left, right>"),"E_SYNTAX"),
        (RESULT_NEW.replace("is Ok<value>","is Ok"),"E_ARITY"),
    ] {
        let err=compile(&src).expect_err("invalid pattern must not create a Wasm guest");
        assert_eq!(err.code,expected,"{src}: {err}");
    }
}
#[test]
fn is_adt_never_synthesizes_unimplemented_process_stdio() {
    let src=r#"pub susp fnc main(): int {
      val input=stdio.in;
      return match << input over
        on is Ok<line> -> 0,
        on is Err<error> -> 1,
      end;
    }"#;
    assert!(compile(src).is_err(),
        "pattern grammar support cannot grant process stdin capabilities in native LLVM");
}
