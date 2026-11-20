use atto_compiler::{compile, lexer::{lex, Tok}, parser::Parser};

#[test]
fn equivalent_return_first_scalar_headers_compile_to_guest_wasm() {
    for header in [
        "int <- main()", "int• main()",
        "fnc int <- main()", "fnc int • main()",
        "pub fnc int • main()",
    ] {
        for body in [
            "{ return 42; }",
            "begin return 42; end",
            "return 42; end",
            "begin\n return 42;\nend.",
        ] {
            let source = format!("{header} {body}");
            let program = Parser::new(lex(&source).expect("lex")).parse_program()
                .expect(&source);
            assert_eq!(program.funcs.len(), 1, "{source}");
            assert_eq!(program.funcs[0].name, "main");
            assert_eq!(program.funcs[0].ret.name(), "int");
            let guest = compile(&source).expect(&source);
            assert!(guest.starts_with(b"\0asm"), "{source}");
        }
    }
}

#[test]
fn return_first_rejects_lookalikes_split_arrow_and_duplicate_return() {
    for source in [
        "int · main() { return 42; }",
        "int ∙ main() { return 42; }",
        "int < - main() { return 42; }",
        "int• main() -> int { return 42; }",
        "int <- main(): int { return 42; }",
        "int • main() begin return 42;",
        "int • main() { return 42;",
    ] {
        assert!(compile(source).is_err(), "must reject {source}");
    }
}

#[test]
fn return_first_does_not_steal_less_than_and_unary_minus() {
    let toks = lex("fnc main(): bool { return 1<-2; }").expect("lex");
    assert!(toks.windows(3).any(|w|
        w[0].tok == Tok::Int(Some(1))
            && w[1].tok == Tok::Lt
            && w[2].tok == Tok::Minus));
}
