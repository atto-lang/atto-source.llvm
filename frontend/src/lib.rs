//! AttoLang browser compiler: `.atto` source -> WebAssembly, itself built for
//! `wasm32-unknown-unknown` so the playground compiles entirely client-side.
//!
//! Syntax follows `docs/grammar.ebnf` of the Java reference compiler; `int`
//! follows the LLVM backend and spec chapter 17 (signed 64-bit, checked).
//! This is a subset compiler: every construct outside the admitted subset is
//! rejected with a diagnostic and never produces an artifact. See README.md.

pub mod ast;
/// Pure bounded typed-IR DATA send-effect closure, shared with peer frontend.
pub mod actor_send_effect_graph;
/// Resolved call graph parking analysis, shared with Rust backend; no guest lowering.
pub mod runtime_parking_effects;
pub mod check;
pub mod diag;
pub mod directed_pipe;
pub mod generator_cfg;
pub mod generator_frame;
pub mod lexer;
pub mod parser;
mod struct_value_guest;
mod native_scalar_c;
mod physical_layout;
mod physical_placement;
pub mod target_syntax;
pub mod target_typed;
pub mod wasm;

pub use diag::{Diagnostic, Pos};

pub const SOURCE_CAP: usize = 64 * 1024;
pub const ABI_VERSION: u32 = 2;
pub const COMPILER_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Compile one `.atto` source file to a guest WebAssembly module.
pub fn compile(source: &str) -> Result<Vec<u8>, Diagnostic> {
    compile_target(source, false, false)
}

/// Explicit isolated core-wasm.i64-v1 profile; the browser ABI is unchanged.
pub fn compile_isolated_core(source: &str) -> Result<Vec<u8>, Diagnostic> {
    compile_target(source, false, true)
}

/// Restricted attached nominal scalar source -> checked C11 (Clang GNU expressions).
/// The caller must compile/link this text; this is not the independent C++ frontend.
pub fn compile_struct_native_c(source: &str) -> Result<String, Diagnostic> {
    String::from_utf8(compile_target(source, true, false)?).map_err(|_| native_scalar_c::unsupported())
}

/// Explicit closed source-defined physical placement -> native C/Clang lane.
pub fn compile_physical_placement_c(source:&str)->Result<String,Diagnostic>{
    physical_placement::compile(source)
}

fn compile_target(source: &str, native_c: bool, isolated: bool) -> Result<Vec<u8>, Diagnostic> {
    if source.len() > SOURCE_CAP {
        return Err(Diagnostic::new(
            Pos { line: 1, col: 1 },
            "E_SOURCE_LIMIT",
            format!("source exceeds {SOURCE_CAP} UTF-8 bytes"),
        ));
    }
    let toks = lexer::lex(source)?;
    if let Some(pos) = directed_pipe::unlowered_pos(&toks) {
        return Err(Diagnostic::new(pos, "E_UNSUPPORTED_CONTINUOUS_PIPE",
            "typed pipe/rpipe DATA graph is planned but no guest transport or PipeHandle exists"));
    }
    // Attached nominal struct syntax has its own target AST. Never route it
    // through identity-bearing class or structural shape lowering. Locate
    // top-level declaration headers using lexer tokens, not source text.
    let mut depth = 0usize;
    let mut pending_struct = None;
    for (i, token) in toks.iter().enumerate() {
        if depth == 0 && (matches!(toks.get(i..i.saturating_add(5)),
            Some([lexer::Token { tok: lexer::Tok::Ident(a), .. },
                  lexer::Token { tok: lexer::Tok::Ident(_), .. },
                  lexer::Token { tok: lexer::Tok::Assign, .. },
                  lexer::Token { tok: lexer::Tok::Ident(b), .. },
                  lexer::Token { tok: lexer::Tok::LBrace, .. }])
                if a == "type" && b == "struct")
            || matches!(toks.get(i..i.saturating_add(4)),
                Some([lexer::Token { tok: lexer::Tok::Ident(a), .. },
                      lexer::Token { tok: lexer::Tok::Ident(b), .. },
                      lexer::Token { tok: lexer::Tok::Ident(_), .. },
                      lexer::Token { tok: lexer::Tok::LBrace, .. }])
                    if a == "define" && b == "struct")) {
            pending_struct = Some(i);
        }
        match &token.tok {
            lexer::Tok::LBrace => depth += 1,
            lexer::Tok::RBrace => {
                depth = depth.saturating_sub(1);
                if depth == 0 {
                    if let Some(start) = pending_struct.take() {
                        if matches!(toks.get(i+1).map(|t| &t.tok),
                            Some(lexer::Tok::Ident(w)) if w == "with") {
                            if start != 0 {
                                return Err(Diagnostic::new(toks[start].pos, "E_UNSUPPORTED",
                                    "attached struct execution requires leading file-local declarations"));
                            }
                            let program = struct_value_guest::lower(toks.clone())?;
                            let typed = check::check(&program)?;
                            if typed.heap || typed.strings || !typed.actors.is_empty()
                                || !typed.generators.is_empty() || !typed.globals.is_empty() {
                                return Err(Diagnostic::new(toks[start].pos, "E_UNSUPPORTED",
                                    "scalar struct lowering must not allocate or use actor/generator/global state"));
                            }
                            if native_c { return native_scalar_c::emit(&typed).map(String::into_bytes); }
                            let bytes = if isolated { wasm::emit_isolated_core(&typed)? } else { wasm::emit(&typed) };
                            if bytes.len() > wasm::MAX_OUTPUT {
                                return Err(Diagnostic::new(toks[start].pos, "E_OUTPUT_LIMIT",
                                    "generated module exceeds 1 MiB"));
                            }
                            return Ok(bytes);
                        }
                    }
                }
            }
            _ => {}
        }
    }
    if native_c { return Err(native_scalar_c::unsupported()); }
    // Detect target declarations at any file-scope position, not only at
    // source byte zero. Ignore comments/strings (the lexer already tokenizes
    // them) and ignore nested brace bodies so a string or local expression
    // cannot accidentally switch the compiler into the target grammar.
    let mut braces = 0usize;
    let mut target = false;
    for i in 0..toks.len() {
        match &toks[i].tok {
            lexer::Tok::LBrace => braces = braces.saturating_add(1),
            lexer::Tok::RBrace => braces = braces.saturating_sub(1),
            lexer::Tok::Ident(word) if braces == 0 => {
                let next = toks.get(i + 1).map(|token| &token.tok);
                if word == "declare" || word == "mtd" ||
                    (word == "use" && matches!(next,
                        Some(lexer::Tok::Ident(name)) if name == "namespace")) {
                    target = true;
                    break;
                }
            }
            _ => {}
        }
    }
    // A protected actor attachment must never become an ordinary field
    // during the scalar/actor guest compiler's fallback parsing.
    let attached_actor_state = toks.windows(5).any(|w|
        matches!((&w[0].tok,&w[1].tok,&w[2].tok,&w[3].tok,&w[4].tok),
            (lexer::Tok::Ident(a),lexer::Tok::Ident(b),
             lexer::Tok::Ident(c),lexer::Tok::Ident(d),lexer::Tok::Ident(_))
                if a=="define"&&b=="state"&&c=="on"&&d=="actor"));
    if attached_actor_state {
        let headers = target_syntax::parse_target_declarations(&toks)?;
        if !headers.actor_state_attachments.is_empty() {
            return Err(Diagnostic::new(toks[0].pos, "E_UNSUPPORTED",
                "attached actor state is parsed but its guarded native/guest runtime is unavailable"));
        }
    }
    if target {
        let headers = target_syntax::parse_target_declarations(&toks)?;
        // Type the declared method signatures without handing unlinked actor
        // declarations to the executable check / Wasm backend.
        let typed = target_typed::lower_target_signatures(&headers)?;
        let actor_ast = target_typed::build_actor_program(&headers, &typed);
        check::check_target_actor_interface(&actor_ast)?;
        return Err(Diagnostic::new(toks[0].pos, "E_UNSUPPORTED",
            "namespace-scoped actor declarations and mtd methods are syntax-only, not browser-lowered"));
    }
    let program = parser::Parser::new(toks).parse_program()?;
    let typed = check::check(&program)?;
    let bytes = if isolated { wasm::emit_isolated_core(&typed)? } else { wasm::emit(&typed) };
    if bytes.len() > wasm::MAX_OUTPUT {
        return Err(Diagnostic::new(
            Pos { line: 1, col: 1 },
            "E_OUTPUT_LIMIT",
            "generated module exceeds 1 MiB",
        ));
    }
    Ok(bytes)
}

/// Browser host ABI (`atto-playground` compiler contract v2). One instance is
/// owned by one worker; callers copy results out before compiling again.
#[cfg(target_arch = "wasm32")]
mod abi {
    use super::*;
    use std::cell::RefCell;

    const ERROR_CAP: usize = 512;

    struct State {
        bytes: Vec<u8>,
        error: [u8; ERROR_CAP],
        line: u32,
        col: u32,
    }

    thread_local! {
        static STATE: RefCell<State> = const { RefCell::new(State { bytes: Vec::new(), error: [0; ERROR_CAP], line: 0, col: 0 }) };
    }
    static mut INPUT: [u8; SOURCE_CAP] = [0; SOURCE_CAP];

    #[no_mangle]
    pub extern "C" fn abi_version() -> u32 {
        ABI_VERSION
    }
    #[no_mangle]
    pub extern "C" fn input_ptr() -> u32 {
        std::ptr::addr_of_mut!(INPUT) as *mut u8 as usize as u32
    }
    #[no_mangle]
    pub extern "C" fn input_capacity() -> u32 {
        SOURCE_CAP as u32
    }
    #[no_mangle]
    pub extern "C" fn result_ptr() -> u32 {
        STATE.with(|s| s.borrow().bytes.as_ptr() as usize as u32)
    }
    #[no_mangle]
    pub extern "C" fn result_length() -> u32 {
        STATE.with(|s| s.borrow().bytes.len() as u32)
    }
    #[no_mangle]
    pub extern "C" fn error_ptr() -> u32 {
        STATE.with(|s| s.borrow().error.as_ptr() as usize as u32)
    }
    #[no_mangle]
    pub extern "C" fn error_line_number() -> u32 {
        STATE.with(|s| s.borrow().line)
    }
    #[no_mangle]
    pub extern "C" fn error_column_number() -> u32 {
        STATE.with(|s| s.borrow().col)
    }

    /// Returns 0 on success, 1 on a diagnostic. On error `result_length` is 0.
    #[no_mangle]
    pub extern "C" fn compile(length: i32) -> i32 {
        STATE.with(|s| {
            let mut st = s.borrow_mut();
            st.bytes = Vec::new();
            st.error = [0; ERROR_CAP];
            st.line = 0;
            st.col = 0;
            let fail = |st: &mut State, d: Diagnostic| {
                let msg = d.render();
                let mut n = msg.len().min(ERROR_CAP - 1);
                while !msg.is_char_boundary(n) {
                    n -= 1;
                }
                st.error[..n].copy_from_slice(&msg.as_bytes()[..n]);
                st.line = d.pos.line;
                st.col = d.pos.col;
                1
            };
            if length < 0 || length as usize > SOURCE_CAP {
                return fail(
                    &mut st,
                    Diagnostic::new(
                        Pos { line: 1, col: 1 },
                        "E_SOURCE_LIMIT",
                        format!("source exceeds {SOURCE_CAP} UTF-8 bytes"),
                    ),
                );
            }
            // SAFETY: the checked length is within the initialized static INPUT buffer.
            // The single-threaded Wasm ABI does not allow host writes while compile runs.
            // Form only the requested byte slice, not an implicit reference to the
            // entire raw array. This keeps dangerous_implicit_autorefs enabled.
            let input: &[u8] = unsafe {
                std::slice::from_raw_parts(std::ptr::addr_of!(INPUT).cast::<u8>(), length as usize)
            };
            let source = match std::str::from_utf8(input) {
                Ok(s) => s,
                Err(_) => {
                    return fail(
                        &mut st,
                        Diagnostic::new(Pos { line: 1, col: 1 }, "E_UTF8", "source is not valid UTF-8"),
                    )
                }
            };
            match super::compile(source) {
                Ok(bytes) => {
                    st.bytes = bytes;
                    0
                }
                Err(d) => fail(&mut st, d),
            }
        })
    }
}

#[cfg(test)]
mod directional_pipe_guest_gate_tests {
    use super::*;
    #[test]
    fn rejects_continuous_pipe_and_rpipe_without_guest_runtime() {
        let samples = [
            "pub fnc main(): int { val c = producer pipe consumer; return 0; }",
            "pub fnc main(): int { val c = consumer rpipe producer; return 0; }",
            "define actor Producer as end pub fnc main(): int { val c = spawn Producer() pipe spawn Producer(); return 0; }",
        ];
        for source in samples {
            let d = compile(source).expect_err("continuous graph cannot reach wasm codegen");
            assert_eq!(d.code, "E_UNSUPPORTED_CONTINUOUS_PIPE", "{source}");
        }
    }
    #[test]
    fn all_twelve_exact_canonical_pipeline_guests_fail_before_wasm_codegen() {
        // These are byte-identical atto-platform-conformance #102 fixtures.
        // This backend has no actual typed continuous guest runtime yet.
        for (name, source) in [
            ("actor-forward", include_str!("../../fixtures/pipe-rpipe-v1/actor-forward.atto")),
            ("actor-reverse", include_str!("../../fixtures/pipe-rpipe-v1/actor-reverse.atto")),
            ("actor-missing-emits", include_str!("../../fixtures/pipe-rpipe-v1/actor-missing-emits.atto")),
            ("actor-wrong-direction", include_str!("../../fixtures/pipe-rpipe-v1/actor-wrong-direction.atto")),
            ("mixed-channel-stream-forward", include_str!("../../fixtures/pipe-rpipe-v1/mixed-channel-stream-forward.atto")),
            ("mixed-channel-stream-reverse", include_str!("../../fixtures/pipe-rpipe-v1/mixed-channel-stream-reverse.atto")),
            ("actor-duplicate-direction", include_str!("../../fixtures/pipe-rpipe-v1/actor-duplicate-direction.atto")),
            ("actor-mixed-directions", include_str!("../../fixtures/pipe-rpipe-v1/actor-mixed-directions.atto")),
            ("stopped-source", include_str!("../../fixtures/pipe-rpipe-v1/stopped-source.atto")),
            ("closed-channel", include_str!("../../fixtures/pipe-rpipe-v1/closed-channel.atto")),
            ("stopped-consumer", include_str!("../../fixtures/pipe-rpipe-v1/stopped-consumer.atto")),
            ("stopped-actor-sink", include_str!("../../fixtures/pipe-rpipe-v1/stopped-actor-sink.atto")),
        ] {
            let d=compile(source).expect_err("unsupported pipeline cannot emit Wasm");
            assert_eq!(d.code,"E_UNSUPPORTED_CONTINUOUS_PIPE",
                "{name}: must reject without accepting untyped DATA graphs");
        }
    }

    #[test]
    fn commented_words_are_not_continuous_pipeline_tokens() {
        let source = "// pipe rpipe\npub fnc main(): int { return 42; }";
        assert!(compile(source).is_ok());
    }
}
