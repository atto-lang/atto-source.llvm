//! Native source-compiler negative admission; distinct from native guest execution.
//! This verifier is intentionally fail-closed: a generic parse error is NOT
//! the intended operator, immutability or effect-modifier diagnostic.
#[cfg(not(test))]
use std::env;
#[cfg(not(test))]
use std::fs;
use std::path::Path;
#[cfg(not(test))]
use std::path::PathBuf;
use std::process::{Command, ExitStatus, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};
use std::io::Read;

#[derive(Clone, Copy)]
struct Case {
    name: &'static str,
    path: &'static str,
    git_blob: &'static str,
}
const CASES: [Case; 8] = [
    Case { name:"removed-modulo", path:"contracts/atto-scalar-v1/negative/removed-modulo.atto",
        git_blob:"45d5794870e93074d9b11b90833a849e96aaa4c1" },
    Case { name:"removed-caret", path:"contracts/atto-scalar-v1/negative/removed-caret.atto",
        git_blob:"6889c7e83c8d94b906fbfe78723caee9580da6aa" },
    Case { name:"removed-double-caret", path:"contracts/atto-scalar-v1/negative/removed-double-caret.atto",
        git_blob:"7ee99948018e927cce74c9ad0edc1975e4251689" },
    Case { name:"type-first-reassignment", path:"contracts/atto-scalar-v1/negative/type-first-reassignment.atto",
        git_blob:"b118f53e68565dcb9e6acd58deb8ee47dca67606" },
    Case { name:"typed-const-reassignment", path:"contracts/atto-scalar-v1/negative/typed-const-reassignment.atto",
        git_blob:"784ebd47a50211e6f00cb732cc74319ed1e58fac" },
    Case { name:"legacy-async-modifier", path:"tests/formal-policy-atto/negative/legacy-async-modifier.atto",
        git_blob:"26d392c8f609a730e81b503d9495de7e29b46755" },
    Case { name:"critical-suspends-modifier", path:"tests/formal-policy-atto/negative/critical-suspends-modifier.atto",
        git_blob:"6be781ff89f9ebc48ee5870d3401943d50b0f9db" },
    Case { name:"suspends-critical-modifier", path:"tests/formal-policy-atto/negative/suspends-critical-modifier.atto",
        git_blob:"c8bb7c921749f06e9d187a6dfbd71685952e3d88" },
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
    for line in d.lines() {
    let removed = || line.contains("operator") &&
        any_phrase(&line, &["removed", "unsupported", "forbidden", "not allowed", "invalid"]);
    let accepted = match c.name {
        "removed-modulo" =>
            (line.contains('%') || line.contains("modulo")) && removed(),
        "removed-caret" =>
            (line.contains('^') || line.contains("caret"))
                && !line.contains("^^") && !any_phrase(&line, &["double caret", "double-caret"])
                && removed(),
        "removed-double-caret" =>
            (line.contains("^^") || any_phrase(&line, &["double caret", "double-caret"])) && removed(),
        "type-first-reassignment" | "typed-const-reassignment" =>
            word(&line, "x") &&
            any_phrase(&line, &["immutable", "readonly", "read-only", "constant binding",
                "const binding", "const variable"]) &&
            any_phrase(&line, &["assign", "rebind", "mutation"]),
        "legacy-async-modifier" =>
            word(&line, "async") &&
            any_phrase(&line, &["unsupported", "forbidden", "disallowed",
                "not allowed", "deprecated", "use suspends", "invalid modifier"]),
        "critical-suspends-modifier" | "suspends-critical-modifier" =>
            word(&line, "critical") && word(&line, "suspends") &&
            any_phrase(&line, &["invalid", "forbidden", "disallowed", "cannot",
                "not allowed", "incompatible", "mutually exclusive"]),
        _ => false
    };
    if accepted { return true; }
    }
    false
}
const MAX_COMPILER_STREAM_BYTES: usize = 65536;
// Bound each compiler stream in memory. If the child exceeds the cap the
// reader closes its pipe; this cannot become a valid negative receipt.
fn read_bounded(reader: impl Read) -> Result<Vec<u8>, String> {
    let mut data = Vec::new();
    reader.take((MAX_COMPILER_STREAM_BYTES + 1) as u64)
        .read_to_end(&mut data).map_err(|_| "compiler stream read failed".to_owned())?;
    if data.len() > MAX_COMPILER_STREAM_BYTES {
        return Err("compiler stream exceeded 64KiB cap".into());
    }
    Ok(data)
}
fn compile_bounded(compiler: &Path, root: &Path, source: &Path,
        deadline: Duration) -> Result<(ExitStatus, Vec<u8>, Vec<u8>), String> {
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
            Ok(None) if start.elapsed() < deadline =>
                thread::sleep(Duration::from_millis(10)),
            Ok(None) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err("compiler deadline exceeded".into());
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
fn safe_fixture_path(path: &str) -> bool {
    !path.starts_with('/') && !path.contains('\\') && path.ends_with(".atto") &&
        path.split('/').all(|part| !part.is_empty() && part != "." && part != "..")
}
#[cfg(not(test))]
fn verify_source(root: &Path, c: &Case) -> Result<PathBuf, String> {
    let path = root.join(c.path);
    if !safe_fixture_path(c.path) {
        return Err(format!("{}: unsafe path", c.name));
    }
    let parts: Vec<&str> = c.path.split('/').collect();
    let mut partial = root.to_path_buf();
    for (i, segment) in parts.iter().enumerate() {
        partial.push(segment);
        let meta = fs::symlink_metadata(&partial)
            .map_err(|_| format!("{}: missing source", c.name))?;
        if meta.file_type().is_symlink() { return Err(format!("{}: symlink", c.name)); }
        if i + 1 == parts.len() {
            if !meta.file_type().is_file() {
                return Err(format!("{}: non-regular source", c.name));
            }
        } else if !meta.file_type().is_dir() {
            return Err(format!("{}: non-directory source prefix", c.name));
        }
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
        let (exit, stdout, stderr_bytes) = compile_bounded(compiler, &root, &source,
            Duration::from_secs(20))
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
        assert!(!matches_policy(c, "error: immutable x in unrelated write scope"));
        assert!(matches_policy(c, "error: immutable binding x cannot reassign"));
    }
    #[test]
    fn every_semantic_family_rejects_generic_lexer_and_parser_messages() {
        let decoys = [
            "error: unexpected token critical suspends",
            "error: unsupported character ^^",
            "error: unknown identifier x",
            "error: unexpected const binding x immutable",
            "error: unresolved identifier async",
            "error: internal error: critical suspends forbidden",
        ];
        for c in CASES {
            for fake in decoys {
                assert!(!matches_policy(&c, fake), "{} credited {fake}", c.name);
            }
        }
    }
    #[test]
    fn each_positive_oracle_rejects_other_semantic_families() {
        let valid = [
            "error: removed modulo operator %",
            "error: caret ^ operator is forbidden",
            "error: double caret ^^ operator is not allowed",
            "error: cannot assign immutable binding x",
            "error: cannot reassign const binding x",
            "error: async modifier is forbidden; use suspends",
            "error: critical suspends fnc is invalid",
            "error: suspends critical fnc is incompatible",
        ];
        for i in 0..CASES.len() {
            assert!(matches_policy(&CASES[i], valid[i]), "{}", CASES[i].name);
            for (j, message) in valid.iter().enumerate() {
                if i == j || ((3..=4).contains(&i) && (3..=4).contains(&j))
                   || (i >= 6 && j >= 6) { continue; }
                assert!(!matches_policy(&CASES[i], message),
                    "{} credited unrelated case {}", CASES[i].name, CASES[j].name);
            }
        }
    }
    #[test]
    fn double_caret_does_not_count_as_single_caret() {
        assert!(!matches_policy(&CASES[1], "error: ^^ removed operator"));
        assert!(!matches_policy(&CASES[2], "error: ^ removed operator"));
        assert!(matches_policy(&CASES[2], "error: double caret ^^ removed operator"));
    }
    #[test]
    fn bounded_reader_enforces_exact_64k_boundary() {
        let within = vec![b'a'; MAX_COMPILER_STREAM_BYTES];
        assert_eq!(read_bounded(std::io::Cursor::new(&within)).unwrap().len(),
            MAX_COMPILER_STREAM_BYTES);
        let too_big = vec![b'b'; MAX_COMPILER_STREAM_BYTES + 1];
        assert!(read_bounded(std::io::Cursor::new(too_big)).is_err());
    }
    #[test]
    fn word_boundaries_prevent_incidental_binding_or_modifier_strings() {
        assert!(!word("syntax", "x"));
        assert!(!word("asyncer", "async"));
        assert!(word("immutable binding x", "x"));
        assert!(!matches_policy(&CASES[6], "error: critical function is invalid"));
        assert!(!matches_policy(&CASES[7], "error: suspends modifier invalid"));
        assert!(!matches_policy(&CASES[6], "error: critical function is invalid\nsource: suspends"));
        assert!(!matches_policy(&CASES[3], "error: immutable binding\nsource: x cannot assign"));
    }
    #[test]
    fn oversized_and_empty_diagnostics_never_count() {
        assert!(!matches_policy(&CASES[0], ""));
        assert!(!matches_policy(&CASES[0], &"error: removed modulo operator".repeat(4000)));
    }

    #[test]
    fn fixture_path_admission_denies_traversal_aliases_and_non_atto_targets() {
        for bad in ["", "/tmp/evil.atto", "../secret.atto",
            "tests//bad.atto", "tests/./bad.atto", "tests/../bad.atto",
            "tests\\bad.atto", "tests/not-source.txt"] {
            assert!(!safe_fixture_path(bad), "{bad}");
        }
        for c in CASES {
            assert!(safe_fixture_path(c.path), "{}", c.name);
        }
    }
    #[cfg(unix)]
    fn mock_compiler(body: &str, label: &str) -> std::path::PathBuf {
        use std::os::unix::fs::PermissionsExt;
        use std::sync::atomic::{AtomicU64, Ordering};
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let serial = NEXT.fetch_add(1, Ordering::Relaxed);
        let script = std::env::temp_dir().join(format!(
            "atto-negative-mock-{}-{}-{}", std::process::id(), label, serial));
        std::fs::write(&script, format!("#!/bin/sh\n{body}\n")).unwrap();
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o700))
            .unwrap();
        script
    }
    #[cfg(unix)]
    #[test]
    fn bounded_compiler_accepts_small_nonzero_failure_for_diagnostic_inspection() {
        let script = mock_compiler("printf 'error: removed modulo operator %%\\n' >&2; exit 7",
            "bounded-good");
        let result = compile_bounded(&script, std::path::Path::new("/tmp"),
            std::path::Path::new("/tmp/input.atto"), Duration::from_secs(2));
        std::fs::remove_file(&script).unwrap();
        let (status, stdout, stderr) = result.unwrap();
        assert_eq!(status.code(), Some(7));
        assert!(stdout.is_empty());
        assert!(String::from_utf8(stderr).unwrap().contains("removed modulo operator"));
    }
    #[cfg(unix)]
    #[test]
    fn infinite_loop_is_killed_by_the_test_deadline() {
        let script = mock_compiler("while :; do :; done", "bounded-hang");
        let result = compile_bounded(&script, std::path::Path::new("/tmp"),
            std::path::Path::new("/tmp/input.atto"), Duration::from_millis(150));
        std::fs::remove_file(&script).unwrap();
        assert!(result.unwrap_err().contains("deadline exceeded"));
    }
    #[cfg(unix)]
    #[test]
    fn oversized_stderr_cannot_masquerade_as_successful_compiler_rejection() {
        let script = mock_compiler("head -c 65537 /dev/zero >&2; exit 7", "bounded-stderr");
        let result = compile_bounded(&script, std::path::Path::new("/tmp"),
            std::path::Path::new("/tmp/input.atto"), Duration::from_secs(2));
        std::fs::remove_file(&script).unwrap();
        assert!(result.unwrap_err().contains("64KiB cap"));
    }
    #[cfg(unix)]
    #[test]
    fn oversized_stdout_never_counts_as_no_ir() {
        let script = mock_compiler("head -c 65537 /dev/zero; exit 7", "bounded-stdout");
        let result = compile_bounded(&script, std::path::Path::new("/tmp"),
            std::path::Path::new("/tmp/input.atto"), Duration::from_secs(2));
        std::fs::remove_file(&script).unwrap();
        assert!(result.unwrap_err().contains("64KiB cap"));
    }

    #[test]
    fn head_and_blob_identifiers_are_not_any_string() {
        assert!(!is_lower_sha40(""));
        assert!(!is_lower_sha40(&"A".repeat(40)));
        assert!(!is_lower_sha40(&"g".repeat(40)));
        assert!(is_lower_sha40(&"a".repeat(40)));
    }
}
