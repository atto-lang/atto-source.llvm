//! Real native actor compiler and guest parity, not host-only evidence.
use std::{fs, process::Command};
const BASE: &str = include_str!("actors/pass/188-critical-read-write.atto");

#[test]
fn exact_aliases_execute_and_wrong_authority_never_emits() {
    let root = std::env::temp_dir().join(format!("atto-state-native-{}", std::process::id()));
    fs::create_dir(&root).unwrap();
    let compiler = env!("CARGO_BIN_EXE_atto-llvm");
    let mut cases = vec![BASE.to_owned()];
    for (w,r) in [("state_w","state_ro"),("handle_state_w","handle_state_ro")] {
        for prefix in ["","self."] {
            cases.push(BASE.replace("self.handle_state(critical-write ", &format!("{prefix}{w}("))
                .replace("self.handle_state(critical-read ", &format!("{prefix}{r}(")));
        }
    }
    let result = (|| {
        for (i,source) in cases.iter().enumerate() {
            let input=root.join(format!("case-{i}.atto"));
            let executable=root.join(format!("guest-{i}"));
            fs::write(&input,source).unwrap();
            let compiled=Command::new(compiler).arg(&input).arg("-o").arg(&executable).output().unwrap();
            assert!(compiled.status.success(), "case {i}: {}",String::from_utf8_lossy(&compiled.stderr));
            let guest=Command::new("timeout").arg("15s").arg(&executable).output().unwrap();
            assert_eq!(guest.status.code(),Some(0), "guest {i}: {}",String::from_utf8_lossy(&guest.stderr));
            let output=String::from_utf8(guest.stdout).unwrap();
            let mut lines:Vec<_>=output.lines().collect();
            lines.sort_unstable();
            assert_eq!(lines,vec!["5","7"]);
        }
        let base=&cases[1];
        for (i,source) in [
            base.replace("state_w(", "state_ro("),
            base.replace("state_w(", "state_w(critical-write "),
            base.replace("state_w(", "other.state_w("),
            base.replace("|s|", "|state_w|").replace("s.count","state_w.count"),
            base.replace("s.count + value","state_ro(|v| -> v.count)"),
            base.replace("increment(int value)", "state_w(int value)"),
        ].iter().enumerate() {
            assert_ne!(source,base);
            let input=root.join(format!("bad-{i}.atto"));
            let ir=root.join(format!("bad-{i}.ll"));
            fs::write(&input,source).unwrap();
            let compiled=Command::new(compiler).arg(&input).arg("-o").arg(&ir).arg("--emit-llvm").output().unwrap();
            assert!(!compiled.status.success(),"negative {i} emitted");
            assert!(!ir.exists(),"negative {i} leaked artifact");
            assert!(!compiled.stderr.is_empty());
        }
    })();
    fs::remove_dir_all(root).unwrap();
    result
}

#[test]
fn nominal_state_field_denials_never_emit_artifacts() {
    let root = std::env::temp_dir().join(format!("atto-state-nominal-{}", std::process::id()));
    fs::create_dir(&root).unwrap();
    let compiler = env!("CARGO_BIN_EXE_atto-llvm");
    let cases = [
        (BASE.replace("s.count = s.count + value", "s.missing = s.count + value"), "E_STATE_LAYOUT"),
        (BASE.replace("s.count + value", "s.missing + value"), "E_STATE_LAYOUT"),
        (BASE.replace("-> s.count", "-> s.missing"), "E_STATE_LAYOUT"),
        (BASE.replace("let int count = 0;", "val int count = 0;"), "E_STATE_LAYOUT"),
        (BASE.replace("mtd (mut self: Counter)", "mtd (self: Counter)"), "E_MUTABILITY"),
    ];
    for (i, (source, diagnostic)) in cases.iter().enumerate() {
        assert_ne!(source, BASE);
        let input = root.join(format!("bad-{i}.atto"));
        let output = root.join(format!("bad-{i}.ll"));
        fs::write(&input, source).unwrap();
        let result = Command::new(compiler).arg(&input).arg("-o").arg(&output)
            .arg("--emit-llvm").output().unwrap();
        let stderr = String::from_utf8_lossy(&result.stderr);
        assert!(!result.status.success(), "case {i} emitted");
        assert!(!output.exists(), "case {i} leaked artifact");
        assert!(stderr.contains(diagnostic), "case {i}: {stderr}");
    }
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn call_shaped_scalar_operands_cannot_escape_protected_callback() {
    let root = std::env::temp_dir().join(format!("atto-state-call-denial-{}", std::process::id()));
    fs::create_dir(&root).unwrap();
    let compiler = env!("CARGO_BIN_EXE_atto-llvm");
    let cases = [
        BASE.replace("s.count + value", "s.count + value(1)")
            .replace("pub fnc main()", "fnc value(int x): int { return x; }\npub fnc main()"),
        BASE.replace("s.count + value", "s.count + s.count()"),
        BASE.replace("s.count + value", "s.count + await value"),
        BASE.replace("s.count + value", "s.count + self.current()"),
        BASE.replace("-> s.count", "-> s.count()"),
    ];
    for (i, source) in cases.iter().enumerate() {
        assert_ne!(source, BASE);
        let input = root.join(format!("bad-{i}.atto"));
        let output = root.join(format!("bad-{i}.ll"));
        fs::write(&input, source).unwrap();
        let result = Command::new(compiler).arg(&input).arg("-o").arg(&output)
            .arg("--emit-llvm").output().unwrap();
        let stderr = String::from_utf8_lossy(&result.stderr);
        assert!(!result.status.success(), "case {i} emitted");
        assert!(!output.exists(), "case {i} leaked artifact");
        let diagnostic = if i == 2 || i == 3 { "E_BORROW" } else { "E_UNSUPPORTED_CRITICAL" };
        assert!(stderr.contains(diagnostic), "case {i}: {stderr}");
    }
    fs::remove_dir_all(root).unwrap();
}
