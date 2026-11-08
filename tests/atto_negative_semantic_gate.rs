//! Native source-compiler negative admission; distinct from native guest execution.
//! This verifier is intentionally fail-closed: a generic parse error is NOT
//! the intended operator, immutability or effect-modifier diagnostic.
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

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
        subjects:&["%", "modulo"], policies:&["operator", "removed", "forbidden", "unsupported"] },
    Case { name:"removed-caret", path:"contracts/atto-scalar-v1/negative/removed-caret.atto",
        git_blob:"6889c7e83c8d94b906fbfe78723caee9580da6aa",
        subjects:&["^", "caret"], policies:&["operator", "removed", "forbidden", "unsupported"] },
    Case { name:"removed-double-caret", path:"contracts/atto-scalar-v1/negative/removed-double-caret.atto",
        git_blob:"7ee99948018e927cce74c9ad0edc1975e4251689",
        subjects:&["^^", "caret"], policies:&["operator", "removed", "forbidden", "unsupported"] },
    Case { name:"type-first-reassignment", path:"contracts/atto-scalar-v1/negative/type-first-reassignment.atto",
        git_blob:"b118f53e68565dcb9e6acd58deb8ee47dca67606",
        subjects:&["x", "binding", "value"], policies:&["immutable", "readonly", "const", "reassign", "cannot assign"] },
    Case { name:"typed-const-reassignment", path:"contracts/atto-scalar-v1/negative/typed-const-reassignment.atto",
        git_blob:"784ebd47a50211e6f00cb732cc74319ed1e58fac",
        subjects:&["x", "binding", "const"], policies:&["immutable", "readonly", "const", "reassign", "cannot assign"] },
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
fn git(root: &Path, args: &[&str]) -> Result<String, String> {
    let output = Command::new("/usr/bin/git").current_dir(root).args(args)
        .output().map_err(|e| format!("git invocation: {e}"))?;
    if !output.status.success() { return Err("git identity check failed".into()); }
    String::from_utf8(output.stdout).map(|x| x.trim().to_owned())
        .map_err(|_| "non-utf8 git output".into())
}
fn matches_policy(c: &Case, stderr: &str) -> bool {
    let d = stderr.to_ascii_lowercase();
    c.subjects.iter().any(|word| d.contains(word)) &&
    c.policies.iter().any(|word| d.contains(word)) &&
    !d.contains("internal error") && !d.contains("stack overflow")
}
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
    for case in &CASES {
        let source = verify_source(&root, case)?;
        let result: Output = Command::new(compiler).arg("--target=host").arg(&source)
            .current_dir(&root).output()
            .map_err(|e| format!("{}: compiler launch: {e}", case.name))?;
        let status = result.status.code();
        if !matches!(status, Some(1..=127)) {
            return Err(format!("{}: compiler accepted, crashed, or returned invalid status: {:?}",
                case.name, status));
        }
        if !result.stdout.is_empty() {
            return Err(format!("{}: forbidden input emitted LLVM IR bytes", case.name));
        }
        let stderr = String::from_utf8(result.stderr)
            .map_err(|_| format!("{}: invalid compiler diagnostic encoding", case.name))?;
        if !matches_policy(case, &stderr) {
            return Err(format!("{}: wrong or generic compiler diagnostic", case.name));
        }
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
    }
    #[test]
    fn error_from_wrong_semantic_stage_does_not_qualify() {
        let c = &CASES[6];
        assert!(!matches_policy(c, "error: unsupported pointer operator"));
        assert!(!matches_policy(c, "error: critical function is invalid"));
        assert!(matches_policy(c, "error: critical suspends modifier is forbidden"));
    }
    #[test]
    fn head_and_blob_identifiers_are_not_any_string() {
        assert!(!is_lower_sha40(""));
        assert!(!is_lower_sha40(&"A".repeat(40)));
        assert!(!is_lower_sha40(&"g".repeat(40)));
        assert!(is_lower_sha40(&"a".repeat(40)));
    }
}
