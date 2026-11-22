use atto_compiler::{compile, lexer::lex, target_syntax::parse_target_declarations};

const GOOD: &str = r#"
    define actor Counter as end
    define state on actor Counter as {
        let int count;
        val bool ready: true;
    }
"#;

fn reject(src: &str, code: &str) {
    let diagnostic=compile(src).expect_err("unverified guarded state cannot emit Wasm");
    assert_eq!(diagnostic.code,code,"{src}: {diagnostic}");
}

#[test]
fn nominal_actor_attachment_retains_scalar_schema_but_emits_no_guest() {
    let h=parse_target_declarations(&lex(GOOD).unwrap()).unwrap();
    assert_eq!(h.actor_state_attachments.len(),1);
    let a=&h.actor_state_attachments[0];
    assert_eq!(a.owner,"Counter");
    assert_eq!(a.fields.len(),2);
    assert_eq!(a.fields[0].name,"count");
    assert_eq!(a.fields[0].kind,"int");
    assert!(a.fields[0].writable);
    assert!(!a.fields[0].initialized);
    assert_eq!(a.fields[1].kind,"bool");
    assert!(!a.fields[1].writable);
    assert!(a.fields[1].initialized);
    reject(GOOD,"E_UNSUPPORTED");
}

#[test]
fn inline_external_state_collision_and_invalid_owners_are_refused() {
    for (src,code) in [
        ("define state on actor Missing as { let int count; }","E_UNBOUND"),
        ("define actor Counter as let int count=0; end define state on actor Counter as { let int count; }","E_DUPLICATE"),
        ("define actor Counter as end define state on actor Counter as { let int count; } define state on actor Counter as { let bool ready: true; }","E_DUPLICATE"),
        ("define actor Counter as end define state on actor Counter as {}","E_STATE"),
        ("define actor Counter as end define state on actor Counter as { let int count; let int count; }","E_DUPLICATE"),
        ("define actor Counter as end define state on actor Counter as { val bool ready; }","E_STATE"),
        ("define actor Counter as end define state on actor Counter as { let string count; }","E_UNSUPPORTED_TYPE"),
        ("define actor Counter as end define state on actor Counter as { let int count: 9223372036854775808; }","E_SYNTAX"),
    ] {
        reject(src,code);
    }
}

#[test]
fn forward_actor_and_signed_min_literal_remain_parse_only() {
    let source="declare actor Counter; define state on actor Counter as { let int count = -9223372036854775808; }";
    let h=parse_target_declarations(&lex(source).unwrap()).unwrap();
    assert_eq!(h.actors,["Counter"]);
    assert_eq!(h.actor_state_attachments[0].fields.len(),1);
    assert!(h.actor_state_attachments[0].fields[0].initialized);
    reject(source,"E_UNSUPPORTED");
}
