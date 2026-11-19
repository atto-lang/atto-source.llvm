//! Executable bounded Rust-hosted LLVM-repository frontend compiler gate.
//! This compiles real .atto into a Wasm module; it does NOT prove native
//! LLVM/C++ AOT, actor mailbox delivery, or authenticated supervisor CONTROL.
use atto_compiler::compile;

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
fn unsafe_control_only_and_outbound_effects_fail_before_wasm() {
    for source in [
        ACTOR.replace("receives int as", "receives never as"),
        ACTOR.replace("receives int as", "receives int sends int as"),
        ACTOR.replace("receives int as", "receives int emits int as"),
        ACTOR.replace("receives int as", "sends int as"),
    ] {
        assert_eq!(compile(&source).unwrap_err().code, "E_UNSUPPORTED");
    }
    // Neither duplicate clauses nor erased unions can widen the ingress.
    assert!(compile(&ACTOR.replace("receives int as", "receives int receives int as")).is_err());
    assert!(compile(&ACTOR.replace("receives int as", "receives int | bool as")).is_err());
}
