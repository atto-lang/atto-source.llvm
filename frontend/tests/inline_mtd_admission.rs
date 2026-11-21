//! Parser and actual guest-emission guard for contextual inline `mtd`.
//! This is the LLVM repo's standalone browser-frontend subset, not the
//! independent native C++ LLVM generator or a claim of cross-backend parity.
use atto_compiler::{compile, lexer::lex, parser::Parser};

#[test]
fn inline_class_mtd_is_executable_not_an_external_receiver_declaration() {
    let source = r#"
        define class Calculator as
            mtd answer(): int { return 42; }
        end
        pub fnc main(): int {
            val calc = new Calculator();
            return calc.answer();
        }
    "#;
    let program = Parser::new(lex(source).unwrap()).parse_program().unwrap();
    assert_eq!(program.classes.len(), 1);
    assert_eq!(program.classes[0].methods.len(), 1);
    assert_eq!(program.classes[0].methods[0].name, "answer");
    let wasm = compile(source).expect("inline mtd must use the executable frontend");
    assert!(wasm.starts_with(&[0, 97, 115, 109, 1, 0, 0, 0]));
    assert_eq!(wasm, compile(source).unwrap(), "guest emission must be deterministic");
}

#[test]
fn inline_actor_mtd_parses_as_member_not_external_receiver() {
    let source = r#"
        define actor Worker as
            mtd ping(): int { return 42; }
        end
        pub fnc main(): int { return 0; }
    "#;
    let program = Parser::new(lex(source).unwrap()).parse_program().unwrap();
    assert_eq!(program.actors.len(), 1);
    assert_eq!(program.actors[0].methods.len(), 1);
    assert_eq!(program.actors[0].methods[0].name, "ping");
}

#[test]
fn static_mtd_does_not_bypass_static_fnc_syntax_gate() {
    let source = r#"
        define class Calculator as
            static mtd answer(): int { return 42; }
        end
        pub fnc main(): int { return 0; }
    "#;
    assert!(compile(source).is_err(), "static mtd must fail closed");
}

#[test]
fn names_mtd_and_method_do_not_trigger_external_receiver_mode() {
    let source = r#"
        define class Names as
            mtd(): int { return 10; }
            method(): int { return 20; }
            mtd named(): int { return 12; }
        end
        pub fnc mtd(): int { return 42; }
        pub fnc main(): int {
            val x = new Names();
            return x.mtd() + x.method() + x.named();
        }
    "#;
    let ast = Parser::new(lex(source).unwrap()).parse_program().unwrap();
    let names: Vec<&str> = ast.classes[0].methods.iter()
        .map(|m| m.name.as_str()).collect();
    assert_eq!(names, ["mtd", "method", "named"]);
    let guest = compile(source).expect("valid user callable names must not route to target syntax");
    assert!(guest.starts_with(&[0, 97, 115, 109, 1, 0, 0, 0]));
}

#[test]
fn end_delimited_function_call_named_mtd_uses_executable_parser() {
    let source = r#"
        pub fnc mtd(): int { return 42; }
        pub fnc main() -> int ::
            return mtd();
        end
    "#;
    let guest = compile(source).expect("mtd() call in keyword body is not a receiver declaration");
    assert!(guest.starts_with(&[0, 97, 115, 109, 1, 0, 0, 0]));
}

#[test]
fn top_level_receiver_form_remains_syntax_only() {
    let source = "mtd (self: Calculator) answer(): int { return 42; }";
    let error = compile(source).expect_err("receiver-form methods need separate target lowering");
    assert!(
        matches!(error.code, "E_UNSUPPORTED" | "E_SYNTAX" | "E_UNSUPPORTED_TYPE"),
        "receiver-form source must fail without an executable guest: {error}"
    );
}
