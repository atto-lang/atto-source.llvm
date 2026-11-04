//! Canonical AttoLang scalar v1 gates. These never infer parity from authored tests.
//! No third-party crates: the exact reviewed manifest and all 19 source blobs are pinned.
use std::{
    collections::HashSet,
    fs,
    io::Write,
    path::{Component, Path},
    process::{Command, Output, Stdio},
    thread,
    time::{Duration, Instant},
};
use crate::{require, TempDir};

struct Case {
    id: &'static str,
    polarity: &'static str,
    blob: &'static str,
    exit: Option<i32>,
}
const MANIFEST_BLOB: &str = "43c4d295a237e483f70722bd90ec10d273b73854";
const CASES: [Case; 19] = [
    Case { id: "canonical-add", polarity: "positive", blob: "015f8b0f79aafa796a148d2adeff1574e8d6c491", exit: Some(42) },
    Case { id: "precedence", polarity: "positive", blob: "50414e5e95c8007f066696f55aa75309555b83e2", exit: Some(14) },
    Case { id: "parentheses", polarity: "positive", blob: "9f508169c741486d5cec7510ac84fb761f342def", exit: Some(20) },
    Case { id: "signed-minimum", polarity: "positive", blob: "450ebee951733d3f0b567413e70724155d26ce13", exit: None },
    Case { id: "signed-max-minus-one", polarity: "positive", blob: "c400f440fde2fbc10514ab7cfd2ffe3d19729ee1", exit: None },
    Case { id: "type-first-readonly", polarity: "positive", blob: "75ff4afc98fc65a774611eb7686e893f3d28a6da", exit: Some(42) },
    Case { id: "typed-const-readonly", polarity: "positive", blob: "0325018c54dd3534ddac76dc29724aa61e979683", exit: Some(42) },
    Case { id: "inferred-const-readonly", polarity: "positive", blob: "4fe471862d90ba57e135b93728bd44690ff2c894", exit: Some(42) },
    Case { id: "pure-call", polarity: "positive", blob: "e5ab32f7d8f684d655a83a3df989f16c52090c05", exit: Some(42) },
    Case { id: "type-first-parameter", polarity: "positive", blob: "3d6c16ed04aa86f954461885ec77c8f10b47fb5c", exit: Some(42) },
    Case { id: "checked-add-overflow", polarity: "negative", blob: "eed3113842125b1d2a295a11e3cc01a5c1e3da00", exit: None },
    Case { id: "checked-sub-overflow", polarity: "negative", blob: "3f80e2eff35147984b040eb0a0215b439d20d09b", exit: None },
    Case { id: "checked-mul-overflow", polarity: "negative", blob: "e608942c7e84fddb6ad7ca119a4a11d88aa9434b", exit: None },
    Case { id: "type-first-reassignment", polarity: "negative", blob: "b118f53e68565dcb9e6acd58deb8ee47dca67606", exit: None },
    Case { id: "typed-const-reassignment", polarity: "negative", blob: "784ebd47a50211e6f00cb732cc74319ed1e58fac", exit: None },
    Case { id: "eager-unused-overflow", polarity: "negative", blob: "ff0c520f2bc2efcc3daf032eb61fddc8f0c91d48", exit: None },
    Case { id: "removed-modulo", polarity: "negative", blob: "45d5794870e93074d9b11b90833a849e96aaa4c1", exit: None },
    Case { id: "removed-caret", polarity: "negative", blob: "6889c7e83c8d94b906fbfe78723caee9580da6aa", exit: None },
    Case { id: "removed-double-caret", polarity: "negative", blob: "7ee99948018e927cce74c9ad0edc1975e4251689", exit: None },
];

fn relative(case: &Case) -> String {
    format!("contracts/atto-scalar-v1/{}/{}.atto", case.polarity, case.id)
}

fn regular(root: &Path, relative: &str) -> Result<Vec<u8>, String> {
    let root_meta = fs::symlink_metadata(root).map_err(|e| format!("root metadata: {e}"))?;
    require(root_meta.is_dir() && !root_meta.file_type().is_symlink(),
        "missing or symlinked source root")?;
    let relative_path = Path::new(relative);
    require(!relative_path.is_absolute() && relative_path.components().next().is_some()
        && relative_path.components().all(|c| matches!(c, Component::Normal(_))),
        format!("invalid relative source path: {relative}"))?;
    let mut path = root.to_path_buf();
    for component in relative_path.components() {
        path.push(component.as_os_str());
        let meta = fs::symlink_metadata(&path)
            .map_err(|e| format!("missing component {}: {e}", path.display()))?;
        require(!meta.file_type().is_symlink(),
            format!("symlinked source component: {}", path.display()))?;
    }
    let meta = fs::symlink_metadata(&path).map_err(|e| e.to_string())?;
    require(meta.is_file() && meta.len() > 0 && meta.len() <= 131_072,
        format!("missing, oversized or nonregular source: {}", path.display()))?;
    fs::read(path).map_err(|e| format!("source read failed: {e}"))
}

// Git's blob hash includes the "blob <length>\0" header. Using --stdin avoids
// filters and reading a second, potentially changed, pathname after nofollow checks.
fn git_blob(bytes: &[u8]) -> Result<String, String> {
    let mut child = Command::new("git")
        .args(["hash-object", "--no-filters", "--stdin"])
        .stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped())
        .spawn().map_err(|e| format!("git hash-object unavailable: {e}"))?;
    child.stdin.take().ok_or("missing git stdin")?
        .write_all(bytes).map_err(|e| format!("write blob to git: {e}"))?;
    let out = child.wait_with_output().map_err(|e| format!("wait git: {e}"))?;
    require(out.status.success() && out.stderr.is_empty(),
        format!("git hash-object failure: {}", String::from_utf8_lossy(&out.stderr)))?;
    let hash = String::from_utf8(out.stdout).map_err(|e| format!("git hash utf8: {e}"))?;
    let hash = hash.trim().to_owned();
    require(hash.len() == 40 && hash.bytes().all(|x| x.is_ascii_hexdigit()),
        "malformed git blob hash")?;
    Ok(hash)
}

fn checked_bytes(root: &Path, relative: &str, expected: &str, limit: usize)
    -> Result<Vec<u8>, String>
{
    let bytes = regular(root, relative)?;
    require(bytes.len() <= limit, format!("source over budget: {relative}"))?;
    require(git_blob(&bytes)? == expected,
        format!("source Git blob mismatch: {relative}"))?;
    Ok(bytes)
}

pub fn source_only(root: &Path) -> Result<(), String> {
    checked_bytes(root, "contracts/atto-scalar-v1/cases.json", MANIFEST_BLOB, 131_072)?;
    let mut ids = HashSet::new();
    let mut positive = 0;
    let mut negative = 0;
    for case in &CASES {
        require(ids.insert(case.id), format!("duplicate scalar id: {}", case.id))?;
        require(case.id.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-'),
            format!("invalid scalar id: {}", case.id))?;
        require(case.polarity == "positive" || case.polarity == "negative",
            "invalid scalar polarity")?;
        if case.polarity == "positive" { positive += 1; } else { negative += 1; }
        checked_bytes(root, &relative(case), case.blob, 8192)?;
    }
    require(positive == 10 && negative == 9 && ids.len() == 19,
        "incomplete canonical 19-case polarity inventory")?;
    println!("ATTO_LLVM_SCALAR_19_SOURCE_ONLY_NOT_EXECUTED");
    Ok(())
}

pub fn self_test(root: &Path) -> Result<(), String> {
    source_only(root)?;
    let temp = TempDir::new("atto-scalar-integrity-").map_err(|e| e.to_string())?;
    let rel_manifest = "contracts/atto-scalar-v1/cases.json";
    let mut paths = vec![rel_manifest.to_owned()];
    paths.extend(CASES.iter().map(relative));
    for rel in &paths {
        let src = regular(root, rel)?;
        let dst = temp.path.join(rel);
        fs::create_dir_all(dst.parent().ok_or("no fixture parent")?)
            .map_err(|e| e.to_string())?;
        fs::write(dst, src).map_err(|e| e.to_string())?;
    }
    source_only(&temp.path)?;
    let first = relative(&CASES[0]);
    let file = temp.path.join(&first);
    fs::write(&file, b"pub fnc main(): int { return 0; }\n").map_err(|e| e.to_string())?;
    require(source_only(&temp.path).is_err(), "modified scalar fixture accepted")?;
    fs::write(&file, regular(root, &first)?).map_err(|e| e.to_string())?;
    let manifest = temp.path.join(rel_manifest);
    fs::write(&manifest, b"{\"forged\":true}\n").map_err(|e| e.to_string())?;
    require(source_only(&temp.path).is_err(), "forged manifest accepted")?;
    fs::write(&manifest, regular(root, rel_manifest)?).map_err(|e| e.to_string())?;
    require(regular(&temp.path, "../escape.atto").is_err(),
        "traversal relative path accepted")?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::symlink;
        fs::remove_file(&file).map_err(|e| e.to_string())?;
        symlink(root.join(&first), &file).map_err(|e| e.to_string())?;
        require(source_only(&temp.path).is_err(), "symlinked scalar fixture accepted")?;
        fs::remove_file(&file).map_err(|e| e.to_string())?;
        fs::write(&file, regular(root, &first)?).map_err(|e| e.to_string())?;
    }
    source_only(&temp.path)?;
    println!("ATTO_SCALAR_SOURCE_INTEGRITY_MUTATION_GATE_PASS");
    Ok(())
}

fn exact_case(id: &str) -> Result<&'static Case, String> {
    CASES.iter().find(|c| c.id == id).ok_or_else(|| format!("unknown scalar case: {id}"))
}

fn checked_compiler(compiler: &Path) -> Result<(), String> {
    let meta = fs::symlink_metadata(compiler)
        .map_err(|e| format!("missing compiler {}: {e}", compiler.display()))?;
    require(meta.is_file() && !meta.file_type().is_symlink(),
        "missing or symlinked native LLVM compiler")
}


// Use disk-backed bounded capture for all source compiler, linker and guest
// subprocesses. std::process::Command::output() can grow RAM unboundedly on
// a malicious compiler; CTest's outer timeout is not a per-process budget.
fn bounded_run(command: &mut Command, work: &Path, seconds: u64,
               label: &str) -> Result<Output, String> {
    const MAX_OUTPUT: u64 = 4 * 1024 * 1024;
    let stdout_path = work.join(".atto-bounded-stdout");
    let stderr_path = work.join(".atto-bounded-stderr");
    let stdout = fs::File::create(&stdout_path)
        .map_err(|e| format!("{label}: create stdout capture: {e}"))?;
    let stderr = fs::File::create(&stderr_path)
        .map_err(|e| format!("{label}: create stderr capture: {e}"))?;
    command.current_dir(work).stdin(Stdio::null())
        .stdout(Stdio::from(stdout)).stderr(Stdio::from(stderr));
    let mut child = command.spawn()
        .map_err(|e| format!("{label}: spawn native process: {e}"))?;
    let started = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait()
            .map_err(|e| format!("{label}: poll child: {e}"))? {
            break status;
        }
        let out_size = fs::metadata(&stdout_path)
            .map_err(|e| format!("{label}: inspect stdout: {e}"))?.len();
        let err_size = fs::metadata(&stderr_path)
            .map_err(|e| format!("{label}: inspect stderr: {e}"))?.len();
        if out_size > MAX_OUTPUT || err_size > MAX_OUTPUT ||
           started.elapsed() > Duration::from_secs(seconds) {
            let _ = child.kill();
            let _ = child.wait();
            let _ = fs::remove_file(&stdout_path);
            let _ = fs::remove_file(&stderr_path);
            return Err(format!("{label}: output budget or {seconds}s deadline exceeded"));
        }
        thread::sleep(Duration::from_millis(10));
    };
    let stdout = fs::read(&stdout_path)
        .map_err(|e| format!("{label}: read stdout: {e}"))?;
    let stderr = fs::read(&stderr_path)
        .map_err(|e| format!("{label}: read stderr: {e}"))?;
    fs::remove_file(&stdout_path)
        .map_err(|e| format!("{label}: cleanup stdout: {e}"))?;
    fs::remove_file(&stderr_path)
        .map_err(|e| format!("{label}: cleanup stderr: {e}"))?;
    require(stdout.len() as u64 <= MAX_OUTPUT &&
            stderr.len() as u64 <= MAX_OUTPUT,
            format!("{label}: captured output exceeded 4MiB"))?;
    Ok(Output { status, stdout, stderr })
}

pub fn removed_operators(compiler: &Path, root: &Path) -> Result<(), String> {
    source_only(root)?;
    checked_compiler(compiler)?;
    let temp = TempDir::new("atto-scalar-rejected-").map_err(|e| e.to_string())?;
    for id in ["removed-modulo", "removed-caret", "removed-double-caret"] {
        let case = exact_case(id)?;
        let source = root.join(relative(case));
        let result = bounded_run(Command::new(compiler).arg(source),
            &temp.path, 20, "native rejected operator")?;
        require(result.status.code() == Some(1) && result.stdout.is_empty()
            && result.stderr.windows(b"ATTO_REMOVED_OPERATOR".len())
                .any(|window| window == b"ATTO_REMOVED_OPERATOR")
            && result.stderr.len() <= 65_536,
            format!("{id}: removed operator emitted IR, crashed, or lacked bounded diagnostic"))?;
        require(fs::read_dir(&temp.path).map_err(|e| e.to_string())?.next().is_none(),
            format!("{id}: rejected source wrote an output artifact"))?;
    }
    println!("ATTO_LLVM_REMOVED_OPERATORS_REJECTED:3");
    Ok(())
}

pub fn guest(compiler: &Path, clang: &Path, root: &Path, id: &str) -> Result<(), String> {
    source_only(root)?;
    checked_compiler(compiler)?;
    let case = exact_case(id)?;
    let expected = case.exit.ok_or_else(|| format!(
        "{id}: signed-i64 extrema/negative source lack a typed-value OS exit oracle"))?;
    require(case.polarity == "positive" && (0..=255).contains(&expected),
        "invalid native executable exit expectation")?;
    let source = root.join(relative(case));
    let input = checked_bytes(root, &relative(case), case.blob, 8192)?;
    let temp = TempDir::new("atto-scalar-native-").map_err(|e| e.to_string())?;
    let compiled = bounded_run(Command::new(compiler).arg(source),
        &temp.path, 25, "native LLVM compiler")?;
    require(compiled.status.success() && compiled.stderr.is_empty()
        && compiled.stdout.starts_with(b"; ModuleID")
        && compiled.stdout.len() <= 8_388_608,
        format!("{id}: native compiler did not produce valid bounded LLVM IR: {}",
            String::from_utf8_lossy(&compiled.stderr)))?;
    let ir = temp.path.join("guest.ll");
    let binary = temp.path.join("guest-native");
    fs::write(&ir, &compiled.stdout).map_err(|e| format!("write LLVM IR: {e}"))?;
    let linked = bounded_run(Command::new(clang).arg("-x").arg("ir").arg(&ir)
        .arg("-o").arg(&binary), &temp.path, 30, "native Clang linker")?;
    require(linked.status.success(),
        format!("{id}: native Clang link failure: {}", String::from_utf8_lossy(&linked.stderr)))?;
    let native = bounded_run(Command::new(&binary), &temp.path, 10,
        "native LLVM-linked guest")?;
    require(native.status.code() == Some(expected)
        && native.stdout.is_empty() && native.stderr.is_empty(),
        format!("{id}: actual native exit {:?} stdout={} stderr={}, expected exit {expected}",
            native.status.code(), native.stdout.len(), native.stderr.len()))?;
    let binary_bytes = fs::read(&binary).map_err(|e| format!("native binary bytes: {e}"))?;
    println!("ATTO_LLVM_COMPILER_BACKED_GUEST_PASS id={id} exit={expected} source_git_blob={} ir_git_blob={} native_git_blob={}",
        git_blob(&input)?, git_blob(&compiled.stdout)?, git_blob(&binary_bytes)?);
    Ok(())
}
