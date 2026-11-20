//! Executable bounded Rust-hosted LLVM-repository frontend compiler gate.
//! This compiles real .atto into a Wasm module; it does NOT prove native
//! LLVM/C++ AOT, actor mailbox delivery, or authenticated supervisor CONTROL.
use atto_compiler::compile;
use atto_compiler::{ast::Type, check, lexer, parser::Parser};

#[test]
fn receives_header_survives_ast_and_checker_revalidates_mutated_metadata() {
    let mut parser = Parser::new(lexer::lex(ACTOR).expect("lex valid actor"));
    let parsed = parser.parse_program().expect("parse valid actor");
    assert_eq!(parsed.actors.len(), 1);
    assert_eq!(parsed.actors[0].receives, Some(Type::Int));
    assert_eq!(parsed.actors[0].sends, None);

    let mut corrupted = parsed.clone();
    corrupted.actors[0].receives = Some(Type::Bool);
    match check::check(&corrupted) {
        Err(err) => assert_eq!(err.code, "E_TYPE"),
        Ok(_) => panic!("changed AST receives metadata must fail closed"),
    }

    corrupted = parsed.clone();
    corrupted.actors[0].receive = None;
    match check::check(&corrupted) {
        Err(err) => assert_eq!(err.code, "E_TYPE"),
        Ok(_) => panic!("removed receive handler must not satisfy original AST contract"),
    }

    let legacy = ACTOR.replace("Worker receives int as", "Worker as");
    let mut parser = Parser::new(lexer::lex(&legacy).expect("lex legacy actor"));
    let legacy = parser.parse_program().expect("parse legacy actor");
    assert_eq!(legacy.actors[0].receives, None);
}


const ACTOR: &str = "define actor Worker receives int as
    receive(ActorMail<int> mail): void { return; }
end
pub fnc main(): int { return 0; }";

#[test]
fn canonical_typed_int_header_compiles_real_wasm_artifact() {
    let wasm = compile(ACTOR).expect("matching ActorMail<int> header must compile");
    assert!(wasm.len() > 8, "no compiler artifact emitted");
    assert_eq!(&wasm[..4], b"\0asm", "compiler did not emit WebAssembly");
    let legacy = ACTOR.replace("Worker receives int as", "Worker as");
    let old = compile(&legacy).expect("legacy header must remain supported");
    assert!(old.len() > 8);
    assert_eq!(&old[..4], b"\0asm");
}

#[test]
fn typed_headers_must_agree_with_exact_concrete_mailbox() {
    let mismatch = ACTOR.replace("receives int as", "receives bool as");
    assert_eq!(compile(&mismatch).unwrap_err().code, "E_TYPE");
    let no_handler = ACTOR.replace(
        "receive(ActorMail<int> mail): void { return; }", "");
    assert_eq!(compile(&no_handler).unwrap_err().code, "E_TYPE");
    let wrong_handler = ACTOR.replace("ActorMail<int>", "ActorMail<bool>");
    assert_eq!(compile(&wrong_handler).unwrap_err().code, "E_TYPE");
}

#[test]
fn checked_outbound_effect_header_and_inert_susp_compile_to_real_wasm() {
    let typed = ACTOR
        .replace("receives int as", "receives int sends int as")
        .replace("receive(ActorMail<int>", "susp receive(ActorMail<int>")
        .replace("pub fnc main()", "pub susp fnc main()");
    let compiled = compile(&typed).expect("a checked scalar actor contract must compile");
    assert!(compiled.starts_with(b"\0asm"), "the compiler must emit real Wasm");
    // Legacy actors may omit receives while still explicitly bounding sends.
    let legacy_inbox = ACTOR.replace("receives int as", "sends int as");
    assert!(compile(&legacy_inbox).is_ok());
}

#[test]
fn unsupported_actor_contracts_fail_closed_before_wasm() {
    for source in [
        ACTOR.replace("receives int as", "receives never as"),
        ACTOR.replace("receives int as", "receives int emits int as"),
    ] {
        assert_eq!(compile(&source).unwrap_err().code, "E_UNSUPPORTED");
    }
    assert_eq!(compile(&ACTOR.replace("receives int as", "receives int sends void as"))
        .unwrap_err().code, "E_ACTOR_SEND_CONTRACT");
    assert_eq!(compile(&ACTOR.replace("receives int as", "receives int sends bool sends int as"))
        .unwrap_err().code, "E_ACTOR_CONTRACT");
    let string_mail = ACTOR
        .replace("receives int as", "receives string sends int as")
        .replace("ActorMail<int>", "ActorMail<string>");
    assert_eq!(compile(&string_mail).unwrap_err().code, "E_UNSUPPORTED_TYPE");
    // Neither duplicate clauses nor erased unions can widen the ingress.
    assert!(compile(&ACTOR.replace("receives int as", "receives int receives int as")).is_err());
    assert!(compile(&ACTOR.replace("receives int as", "receives int | bool as")).is_err());
}
