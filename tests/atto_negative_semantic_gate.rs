//! Native source-compiler negative admission; distinct from native guest execution.
//! This verifier is intentionally fail-closed: a generic parse error is NOT
//! the intended operator, immutability or effect-modifier diagnostic.
#[cfg(not(test))]
use std::env;
#[cfg(not(test))]
use std::fs;
#[cfg(not(test))]
use std::path::{Path, PathBuf};
#[cfg(not(test))]
use std::process::{Command, ExitStatus, Stdio};
#[cfg(not(test))]
use std::sync::mpsc;
#[cfg(not(test))]
use std::thread;
#[cfg(not(test))]
use std::time::{Duration, Instant};
use std::io::Read;

#[derive(Clone, Copy)]
struct Case {
    name: &'static str,
    path: &'static str,
    git_blob: &'static str,
    subjects: &'static [&'static str],
    policies: &'static [&'static str],
}
const CASES: [Case; 8] = [
    Case { name:"removed-modulo", path:"contracts/atto-scalar-v1/negative/removed-modulo.atto",
        git_blob:"45d5794870e93074d9b11b90833a849e96aaa4c1",
        subjects:&["%", "modulo"], policies:&["operator", "removed", "forbidden", "unsupported operator"] },
    Case { name:"removed-caret", path:"contracts/atto-scalar-v1/negative/removed-caret.atto",
        git_blob:"6889c7e83c8d94b906fbfe78723caee9580da6aa",
        subjects:&["^", "caret"], policies:&["operator", "removed", "forbidden", "unsupported operator"] },
    Case { name:"removed-double-caret", path:"contracts/atto-scalar-v1/negative/removed-double-caret.atto",
        git_blob:"7ee99948018e927cce74c9ad0edc1975e4251689",
        subjects:&["^^", "caret"], policies:&["operator", "removed", "forbidden", "unsupported operator"] },
    Case { name:"type-first-reassignment", path:"contracts/atto-scalar-v1/negative/type-first-reassignment.atto",
        git_blob:"b118f53e68565dcb9e6acd58deb8ee47dca67606",
        subjects:&["x", "binding", "value"], policies:&["immutable", "readonly", "constant binding", "reassign", "cannot assign"] },
    Case { name:"typed-const-reassignment", path:"contracts/atto-scalar-v1/negative/typed-const-reassignment.atto",
        git_blob:"784ebd47a50211e6f00cb732cc74319ed1e58fac",
        subjects:&["x", "binding", "const"], policies:&["immutable", "readonly", "constant binding", "reassign", "cannot assign"] },
    Case { name:"legacy-async-modifier", path:"tests/formal-policy-atto/negative/legacy-async-modifier.atto",
        git_blob:"26d392c8f609a730e81b503d9495de7e29b46755",
        subjects:&["async"], policies:&["unsupported", "forbidden", "disallowed", "modifier", "use suspends"] },
    Case { name:"critical-suspends-modifier", path:"tests/formal-policy-atto/negative/critical-suspends-modifier.atto",
        git_blob:"6be781ff89f9ebc48ee5870d3401943d50b0f9db",
        subjects:&["critical", "suspends"], policies:&["forbidden", "disallowed", "cannot", "modifier", "invalid"] },
    Case { name:"suspends-critical-modifier", path:"tests/formal-policy-atto/negative/suspends-critical-modifier.atto",
        git_blob:"c8bb7c921749f06e9d187a6dfbd71685952e3d88",
        subjects:&["suspends", "critical"], policies:&["forbidden", "disallowed", "cannot", "modifier", "invalid"] },
];

fn is_lower_sha40(s: &str) -> bool {
    s.len() == 40 && s.bytes().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
}
#[cfg(not(test))]
fn git(root: &Path, args: &[&str]) -> Result<String, String> {
    let output = Command::new("/usr/bin/git").current_dir(root).args(args)
        .output().map_err(|e| format!("git invocation: {e}"))?;
    if !output.status.success() { return Err("git identity check failed".into()); }
    String::from_utf8(output.stdout).map(|x| x.trim().to_owned())
        .map_err(|_| "non-utf8 git output".into())
}
fn any_phrase(d: &str, terms: &[&str]) -> bool {
    terms.iter().any(|term| d.contains(term))
}
fn word(d: &str, term: &str) -> bool {
    d.split(|ch: char| !ch.is_ascii_alphanumeric() && ch != '_')
        .any(|token| token == term)
}
fn matches_policy(c: &Case, stderr: &str) -> bool {
    if stderr.is_empty() || stderr.len() > 65536 { return false; }
    let d = stderr.to_ascii_lowercase();
    if any_phrase(&d, &["syntax error", "parse error", "unexpected token",
        "unexpected character", "unsupported character", "unexpected const",
        "unrecognized token", "unresolved identifier", "unknown identifier",
        "internal error", "stack overflow"]) { return false; }
    let removed = || d.contains("operator") &&
        any_phrase(&d, &["removed", "unsupported", "forbidden", "not allowed", "invalid"]);
    match c.name {
        "removed-modulo" =>
            (d.contains('%') || d.contains("modulo")) && removed(),
        "removed-caret" =>
            (d.contains('^') || d.contains("caret"))
                && !d.contains("^^") && !any_phrase(&d, &["double caret", "double-caret"])
                && removed(),
        "removed-double-caret" =>
            (d.contains("^^") || any_phrase(&d, &["double caret", "double-caret"])) && removed(),
        "type-first-reassignment" | "typed-const-reassignment" =>
            word(&d, "x") &&
            any_phrase(&d, &["immutable", "readonly", "read-only", "constant binding",
                "const binding", "const variable"]) &&
            any_phrase(&d, &["assign", "rebind", "mutation", "write"]),
        "legacy-async-modifier" =>
            word(&d, "async") &&
            any_phrase(&d, &["unsupported", "forbidden", "disallowed",
                "not allowed", "deprecated", "use suspends", "invalid modifier"]),
        "critical-suspends-modifier" | "suspends-critical-modifier" =>
            word(&d, "critical") && word(&d, "suspends") &&
            any_phrase(&d, &["invalid", "forbidden", "disallowed", "cannot",
                "not allowed", "incompatible", "mutually exclusive"]),
        _ => false
    }
}
const MAX_COMPILER_STREAM_BYTES: usize = 65536;
// Bound each compiler stream in memory. If the child exceeds the cap the
// reader closes its pipe; this cannot become a valid negative receipt.
fn read_bounded(mut reader: impl Read) -> Result<Vec<u8>, String> {
    let mut data = Vec::new();
    reader.take((MAX_COMPILER_STREAM_BYTES + 1) as u64)
        .read_to_end(&mut data).map_err(|_| "compiler stream read failed".to_owned())?;
    if data.len() > MAX_COMPILER_STREAM_BYTES {
        return Err("compiler stream exceeded 64KiB cap".into());
    }
    Ok(data)
}
#[cfg(not(test))]
fn compile_bounded(compiler: &Path, root: &Path, source: &Path)
        -> Result<(ExitStatus, Vec<u8>, Vec<u8>), String> {
    let mut child = Command::new(compiler).arg("--target=host").arg(source)
        .current_dir(root).stdout(Stdio::piped()).stderr(Stdio::piped())
        .spawn().map_err(|e| format!("compiler launch failed: {e}"))?;
    let stdout = child.stdout.take().ok_or("missing child stdout")?;
    let stderr = child.stderr.take().ok_or("missing child stderr")?;
    let (out_tx, out_rx) = mpsc::channel();
    let (err_tx, err_rx) = mpsc::channel();
    thread::spawn(move || { let _ = out_tx.send(read_bounded(stdout)); });
    thread::spawn(move || { let _ = err_tx.send(read_bounded(stderr)); });
    let start = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if start.elapsed() < Duration::from_secs(20) =>
                thread::sleep(Duration::from_millis(10)),
            Ok(None) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err("compiler exceeded 20s limit".into());
            }
            Err(_) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err("compiler wait failed".into());
            }
        }
    };
    let out = out_rx.recv_timeout(Duration::from_secs(2))
        .map_err(|_| "compiler stdout pipe did not close".to_owned())??;
    let err = err_rx.recv_timeout(Duration::from_secs(2))
        .map_err(|_| "compiler stderr pipe did not close".to_owned())??;
    Ok((status, out, err))
}
#[cfg(not(test))]
fn verify_source(root: &Path, c: &Case) -> Result<PathBuf, String> {
    let path = root.join(c.path);
    if !c.path.split('/').all(|s| !s.is_empty() && s != "." && s != "..") {
        return Err(format!("{}: unsafe path", c.name));
    }
    let mut partial = root.to_path_buf();
    for segment in c.path.split('/') {
        partial.push(segment);
        let meta = fs::symlink_metadata(&partial)
            .map_err(|_| format!("{}: missing source", c.name))?;
        if meta.file_type().is_symlink() { return Err(format!("{}: symlink", c.name)); }
    }
    let contents = fs::read(&path).map_err(|e| format!("{}: read failed: {e}", c.name))?;
    if contents.is_empty() || contents.len() > 8192 || std::str::from_utf8(&contents).is_err() {
        return Err(format!("{}: invalid source bytes", c.name));
    }
    let rev = format!("HEAD:{}", c.path);
    if git(root, &["rev-parse", &rev])? != c.git_blob ||
        git(root, &["hash-object", "--no-filters", c.path])? != c.git_blob {
        return Err(format!("{}: source drift from reviewed Git blob", c.name));
    }
    Ok(path)
}
#[cfg(not(test))]
fn run() -> Result<(), String> {
    let root = env::current_dir().map_err(|e| format!("cwd: {e}"))?;
    let expected = env::var("ATTO_NEGATIVE_EXPECTED_HEAD")
        .map_err(|_| "ATTO_NEGATIVE_EXPECTED_HEAD required".to_owned())?;
    if !is_lower_sha40(&expected) || git(&root, &["rev-parse", "HEAD"])? != expected {
        return Err("wrong exact source HEAD; no negative admission credit".into());
    }
    let compiler = env::var("ORESLANG_LLVMC")
        .map_err(|_| "ORESLANG_LLVMC absolute compiler path required".to_owned())?;
    let compiler = Path::new(&compiler);
    if !compiler.is_absolute() || !compiler.is_file() {
        return Err("real LLVM compiler executable missing".into());
    }
    let tree = git(&root, &["rev-parse", "HEAD^{tree}"])?;
    let script = "tests/atto_negative_semantic_gate.rs";
    if git(&root, &["rev-parse", &format!("HEAD:{script}")])?
            != git(&root, &["hash-object", "--no-filters", script])? {
        return Err("native negative gate script differs from exact PR head".into());
    }
    for case in &CASES {
        let source = verify_source(&root, case)?;
        let (exit, stdout, stderr_bytes) = compile_bounded(compiler, &root, &source)
            .map_err(|e| format!("{}: {e}", case.name))?;
        let status = exit.code();
        if !matches!(status, Some(1..=127)) {
            return Err(format!("{}: compiler accepted, crashed, or returned invalid status: {:?}",
                case.name, status));
        }
        if !stdout.is_empty() {
            return Err(format!("{}: forbidden input emitted LLVM IR bytes", case.name));
        }
        let stderr = String::from_utf8(stderr_bytes)
            .map_err(|_| format!("{}: invalid compiler diagnostic encoding", case.name))?;
        if !matches_policy(case, &stderr) {
            return Err(format!("{}: wrong or generic compiler diagnostic", case.name));
        }
        // Do not accept a compiler that mutates its input fixture while parsing.
        verify_source(&root, case)?;
        println!("ATTO_LLVM_NEGATIVE_SOURCE_REJECT|head={expected}|tree={tree}|case={}|blob={}|no_ir=yes",
            case.name, case.git_blob);
    }
    println!("ATTO_LLVM_NEGATIVE_ALL_PASS|cases=8|head={expected}|tree={tree}|scope=source-compile-only");
    Ok(())
}
#[cfg(not(test))]
fn main() {
    if let Err(e) = run() {
        eprintln!("ATTO_LLVM_NEGATIVE_GATE_FAILED: {e}");
        std::process::exit(1);
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn canonical_eight_distinct_pinned_cases() {
        let mut names = std::collections::HashSet::new();
        for c in CASES {
            assert!(names.insert(c.name));
            assert!(c.path.ends_with(".atto"));
            assert!(is_lower_sha40(c.git_blob));
        }
    }
    #[test]
    fn generic_parse_and_unresolved_symbol_errors_do_not_qualify() {
        let c = &CASES[0];
        assert!(!matches_policy(c, "error: unexpected token"));
        assert!(!matches_policy(c, "error: unknown symbol modulo"));
        assert!(matches_policy(c, "error: removed modulo operator"));
        let caret = &CASES[1];
        assert!(!matches_policy(caret, "error: unsupported character ^"));
    }
    #[test]
    fn error_from_wrong_semantic_stage_does_not_qualify() {
        let c = &CASES[6];
        assert!(!matches_policy(c, "error: unsupported pointer operator"));
        assert!(!matches_policy(c, "error: critical function is invalid"));
        assert!(matches_policy(c, "error: critical suspends modifier is forbidden"));
    }
    #[test]
    fn immutable_binding_negative_requires_a_real_binding_diagnostic() {
        let c = &CASES[3];
        assert!(!matches_policy(c, "error: syntax error: unexpected const"));
        assert!(!matches_policy(c, "error: syntax error: x unexpected"));
        assert!(!matches_policy(c, "error: unknown identifier x"));
        assert!(matches_policy(c, "error: immutable binding x cannot reassign"));
    }
    #[test]
    fn head_and_blob_identifiers_are_not_any_string() {
        assert!(!is_lower_sha40(""));
        assert!(!is_lower_sha40(&"A".repeat(40)));
        assert!(!is_lower_sha40(&"g".repeat(40)));
        assert!(is_lower_sha40(&"a".repeat(40)));
    }
}
