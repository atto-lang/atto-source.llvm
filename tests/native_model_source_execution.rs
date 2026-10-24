// Shared-corpus native model source -> LLVM IR -> Clang -> executable proof.
// This is deliberately standalone Rust (no Cargo dependencies, Python, or shell).
// It proves only the model-core-i64-v1 host profile, not general model parity.
use std::collections::HashSet;
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const MAX_CAPTURE: u64 = 2 * 1024 * 1024;
const MAX_SOURCE_BYTES: usize = 2 * 1024 * 1024;
const CHILD_TIMEOUT: Duration = Duration::from_secs(65);
const REVISION: &str = "model-core-i64-v1.2026-10-20.10";
const PROFILE: &str = "model-core-i64-v1";

#[derive(Debug)]
struct Case {
    id: String,
    llvm_mode: String,
    diagnostic: Option<String>,
    llvm_diagnostic: Option<String>,
    expected_exit: Option<i64>,
    source: String,
}

fn decode_hex_ascii(encoded: &str) -> Result<String, String> {
    if encoded.len() % 2 != 0 || encoded.len() / 2 > MAX_SOURCE_BYTES {
        return Err("invalid or oversized source hex length".into());
    }
    let mut decoded = Vec::with_capacity(encoded.len() / 2);
    for pair in encoded.as_bytes().chunks_exact(2) {
        let ascii = std::str::from_utf8(pair).map_err(|_| "non-ASCII source hex")?;
        let byte = u8::from_str_radix(ascii, 16).map_err(|_| "malformed source hex")?;
        if byte > 0x7f {
            return Err("shared model corpus source is not ASCII".into());
        }
        decoded.push(byte);
    }
    String::from_utf8(decoded).map_err(|_| "malformed source bytes".into())
}

fn safe_case_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 96
        && id.as_bytes()[0].is_ascii_alphabetic()
        && id.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')
}

fn optional(value: &str) -> Option<String> {
    if value == "-" { None } else { Some(value.to_owned()) }
}

fn corpus_from_str(raw: &str) -> Result<Vec<Case>, String> {
    let mut lines = raw.lines();
    let expected_revision = format!("revision|{REVISION}");
    let expected_profile = format!("profile|{PROFILE}");
    if lines.next() != Some("ORESLANG_MODEL_EXECUTABLE_CASES_V1")
        || lines.next() != Some(expected_revision.as_str())
        || lines.next() != Some(expected_profile.as_str())
    {
        return Err("invalid or unexpected shared model corpus header/revision/profile".into());
    }
    let mut cases = Vec::new();
    let mut names = HashSet::new();
    for (offset, line) in lines.enumerate() {
        if line.is_empty() || line.starts_with("identity|") {
            continue;
        }
        let fields: Vec<&str> = line.split('|').collect();
        if fields.len() != 12 || fields[0] != "case" {
            return Err(format!("malformed shared model case on line {}", offset + 4));
        }
        if !safe_case_id(fields[1]) || !names.insert(fields[1].to_owned()) {
            return Err(format!("unsafe or duplicate model case ID on line {}", offset + 4));
        }
        let expected_exit = if fields[9] == "-" {
            None
        } else {
            Some(fields[9].parse::<i64>().map_err(|_| "invalid semantic result i64")?)
        };
        if fields[10] != "-" {
            fields[10].parse::<i64>().map_err(|_| "invalid configuration i64")?;
        }
        if !matches!(fields[4], "reject" | "execute" | "compile_only") {
            return Err(format!("unknown LLVM mode for {}", fields[1]));
        }
        cases.push(Case {
            id: fields[1].to_owned(),
            llvm_mode: fields[4].to_owned(),
            diagnostic: optional(fields[5]),
            llvm_diagnostic: optional(fields[6]),
            expected_exit,
            source: decode_hex_ascii(fields[11])?,
        });
    }
    if cases.len() < 20 {
        return Err(format!("shared model corpus unexpectedly small: {}", cases.len()));
    }
    Ok(cases)
}

struct Scratch(PathBuf);

impl Scratch {
    fn new() -> Result<Self, String> {
        let timestamp = SystemTime::now().duration_since(UNIX_EPOCH)
            .map_err(|e| e.to_string())?.as_nanos();
        for attempt in 0..10 {
            let candidate = env::temp_dir().join(format!(
                "ores-model-native-{}-{timestamp}-{attempt}", std::process::id()));
            match fs::create_dir(&candidate) {
                Ok(()) => return Ok(Self(candidate)),
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(format!("create scratch directory: {e}")),
            }
        }
        Err("unable to allocate unique model scratch directory".into())
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn run(command: &mut Command, root: &Path) -> Result<(i32, Vec<u8>, Vec<u8>), String> {
    let stdout_path = root.join("command.stdout");
    let stderr_path = root.join("command.stderr");
    let stdout = File::create(&stdout_path).map_err(|e| e.to_string())?;
    let stderr = File::create(&stderr_path).map_err(|e| e.to_string())?;
    let mut child = command.stdin(Stdio::null())
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr))
        .spawn().map_err(|e| format!("launch command: {e}"))?;
    let start = Instant::now();
    let status = loop {
        match child.try_wait().map_err(|e| format!("wait for command: {e}"))? {
            Some(status) => break status,
            None if start.elapsed() >= CHILD_TIMEOUT => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("command exceeded {}s timeout", CHILD_TIMEOUT.as_secs()));
            }
            None => thread::sleep(Duration::from_millis(20)),
        }
    };
    let code = status.code().ok_or("command terminated by signal rather than exit status")?;
    let out_size = fs::metadata(&stdout_path).map_err(|e| e.to_string())?.len();
    let err_size = fs::metadata(&stderr_path).map_err(|e| e.to_string())?.len();
    if out_size > MAX_CAPTURE || err_size > MAX_CAPTURE {
        return Err("model compiler/tool emitted oversized output".into());
    }
    let out = fs::read(&stdout_path).map_err(|e| e.to_string())?;
    let err = fs::read(&stderr_path).map_err(|e| e.to_string())?;
    Ok((code, out, err))
}

fn contains_bytes(haystack: &[u8], needle: &[u8]) -> bool {
    haystack.windows(needle.len()).any(|part| part == needle)
}

fn verify(compiler: &Path, clang: &Path, corpus: &Path) -> Result<(), String> {
    let cases = corpus_from_str(&fs::read_to_string(corpus).map_err(|e| e.to_string())?)?;
    let scratch = Scratch::new()?;
    for case in &cases {
        let src = scratch.0.join(format!("{}.ores", case.id));
        OpenOptions::new().write(true).create_new(true).open(&src)
            .and_then(|mut file| file.write_all(case.source.as_bytes()))
            .map_err(|e| format!("{}: write source: {e}", case.id))?;
        let (rc, ir, err) = run(Command::new(compiler).arg(&src), &scratch.0)
            .map_err(|e| format!("{}: compiler: {e}", case.id))?;
        if case.llvm_mode == "reject" {
            let diagnostic = case.llvm_diagnostic.as_deref().or(case.diagnostic.as_deref())
                .ok_or_else(|| format!("{}: reject case lacks diagnostic", case.id))?;
            if rc == 0 || !ir.is_empty() || !contains_bytes(&err, diagnostic.as_bytes()) {
                return Err(format!(
                    "{}: expected failure without IR and diagnostic {:?}, rc={}, stdout={} bytes, stderr={:?}",
                    case.id, diagnostic, rc, ir.len(), String::from_utf8_lossy(&err)));
            }
            println!("PASS model reject {}: {}", case.id, diagnostic);
            continue;
        }
        if rc != 0 || !ir.starts_with(b"; ModuleID") {
            return Err(format!(
                "{}: compiler rc={}, expected LLVM IR, stderr={:?}",
                case.id, rc, String::from_utf8_lossy(&err)));
        }
        if case.llvm_mode == "compile_only" {
            if case.id == "price-adjust-i64-minimum"
                && !contains_bytes(&ir, b"-9223372036854775808")
            {
                return Err(format!("{}: signed i64 minimum missing from IR", case.id));
            }
            println!("PASS model compile-only {}", case.id);
            continue;
        }
        let expected = case.expected_exit
            .ok_or_else(|| format!("{}: execute case lacks semantic_result_i64", case.id))?;
        if !(0..=255).contains(&expected) {
            return Err(format!("{}: result {expected} not lossless through OS exit", case.id));
        }
        let ll = scratch.0.join(format!("{}.ll", case.id));
        let executable = scratch.0.join(&case.id);
        fs::write(&ll, &ir).map_err(|e| e.to_string())?;
        let (clang_rc, _, clang_err) = run(
            Command::new(clang).args(["-Wno-override-module", "-x", "ir"])
                .arg(&ll).arg("-o").arg(&executable),
            &scratch.0,
        ).map_err(|e| format!("{}: clang: {e}", case.id))?;
        if clang_rc != 0 {
            return Err(format!(
                "{}: clang rc={}, stderr={:?}",
                case.id, clang_rc, String::from_utf8_lossy(&clang_err)));
        }
        let (exit, _, runtime_err) = run(Command::new(&executable), &scratch.0)
            .map_err(|e| format!("{}: guest: {e}", case.id))?;
        if i64::from(exit) != expected {
            return Err(format!(
                "{}: guest exit={}, expected={}, stderr={:?}",
                case.id, exit, expected, String::from_utf8_lossy(&runtime_err)));
        }
        println!("PASS model guest {}: {}", case.id, expected);
        io::stdout().flush().map_err(|e| e.to_string())?;
    }
    Ok(())
}

fn required_arg(args: &[String], flag: &str) -> Result<PathBuf, String> {
    let positions: Vec<usize> = args.iter().enumerate()
        .filter_map(|(i, value)| (value == flag).then_some(i))
        .collect();
    if positions.len() != 1 || positions[0] + 1 >= args.len() {
        return Err(format!("expected exactly one {flag} PATH"));
    }
    Ok(PathBuf::from(&args[positions[0] + 1]))
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    let result = (|| {
        if args.len() != 6 {
            return Err("usage: harness --compiler PATH --clang PATH --corpus PATH".into());
        }
        let compiler = required_arg(&args, "--compiler")?;
        let clang = required_arg(&args, "--clang")?;
        let corpus = required_arg(&args, "--corpus")?;
        verify(&compiler, &clang, &corpus)
    })();
    if let Err(error) = result {
        eprintln!("FAIL native model source execution: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_malformed_or_non_ascii_source_bytes() {
        assert!(decode_hex_ascii("00ff").is_err());
        assert!(decode_hex_ascii("f").is_err());
        assert!(decode_hex_ascii("GG").is_err());
        assert_eq!(decode_hex_ascii("646566696e65").unwrap(), "define");
    }

    #[test]
    fn refuses_directory_traversal_and_duplicate_identifiers() {
        assert!(!safe_case_id("../outside"));
        assert!(!safe_case_id("a/b"));
        assert!(!safe_case_id(""));
        assert!(safe_case_id("price-adjust-i64-minimum"));
    }

    #[test]
    fn rejects_bad_revision_and_undersized_corpus() {
        assert!(corpus_from_str("wrong\nrevision|wrong\nprofile|wrong").is_err());
        let small = format!(
            "ORESLANG_MODEL_EXECUTABLE_CASES_V1\nrevision|{REVISION}\nprofile|{PROFILE}\n"
        );
        assert!(corpus_from_str(&small).is_err());
    }
}
