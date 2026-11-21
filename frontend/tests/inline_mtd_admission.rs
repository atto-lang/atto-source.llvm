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
