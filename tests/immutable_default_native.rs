// Compiler-backed Oreslang immutable default admission + native guest runner.
// Standalone Rust/std-only harness: no Python, fake compilers, or host arithmetic oracle.
use std::{env, fs, path::{Path, PathBuf}, process::Command, time::{SystemTime, UNIX_EPOCH}};

struct TempDir(PathBuf);
impl TempDir {
    fn create() -> Result<Self, String> {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH)
            .map_err(|e| e.to_string())?.as_nanos();
        let base = env::temp_dir().join(format!("ores-immutable-{}-{nanos}", std::process::id()));
        fs::create_dir(&base).map_err(|e| e.to_string())?;
        Ok(Self(base))
    }
}
impl Drop for TempDir { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); } }

fn check(source: &Path, compiler: &Path, clang: &Path, expected_exit: Option<i32>,
         temp: &TempDir, index: usize) -> Result<(), String> {
    let metadata = fs::symlink_metadata(source).map_err(|e| e.to_string())?;
    if !metadata.file_type().is_file() || metadata.file_type().is_symlink() ||
        metadata.len() > 4096 || metadata.len() == 0 {
        return Err(format!("source is not bounded regular file: {}", source.display()));
    }
    let input = fs::read(source).map_err(|e| e.to_string())?;
    if input.contains(&0) || std::str::from_utf8(&input).is_err() {
        return Err(format!("malformed UTF-8 source: {}", source.display()));
    }
    let compiler_result = Command::new(compiler).arg(source).output().map_err(|e| e.to_string())?;
    match expected_exit {
        None => {
            if compiler_result.status.success() || !compiler_result.stdout.is_empty() ||
               compiler_result.stderr.is_empty() {
                return Err(format!("negative case did not fail before IR publication: {} ({:?})",
                    source.display(), compiler_result.status));
            }
            println!("ADMISSION_REJECT {}", source.display());
            Ok(())
        }
        Some(expected) => {
            if !compiler_result.status.success() || compiler_result.stdout.is_empty() {
                return Err(format!("positive compiler failure: {} status {:?} stderr {}",
                    source.display(), compiler_result.status,
                    String::from_utf8_lossy(&compiler_result.stderr)));
            }
            let ir = temp.0.join(format!("{index}.ll"));
            let exe = temp.0.join(format!("{index}.native"));
            fs::write(&ir, &compiler_result.stdout).map_err(|e| e.to_string())?;
            let link = Command::new(clang).args(["-x","ir"]).arg(&ir)
                .arg("-o").arg(&exe).output().map_err(|e| e.to_string())?;
            if !link.status.success() {
                return Err(format!("clang rejected IR for {}: {}",
                    source.display(), String::from_utf8_lossy(&link.stderr)));
            }
            let guest = Command::new(&exe).output().map_err(|e| e.to_string())?;
            if guest.status.code() != Some(expected) || !guest.stdout.is_empty() {
                return Err(format!("native guest mismatch {} wanted exit {} got {:?} stdout {} stderr {}",
                    source.display(), expected, guest.status,
                    String::from_utf8_lossy(&guest.stdout),
                    String::from_utf8_lossy(&guest.stderr)));
            }
            println!("GUEST_EXECUTED {} exit={expected}", source.display());
            Ok(())
        }
    }
}

fn run() -> Result<(), String> {
    let args: Vec<_> = env::args_os().collect();
    if args.len() != 4 { return Err("usage: immutable-default-runner COMPILER CLANG FIXTURES_DIR".into()); }
    let compiler = PathBuf::from(&args[1]);
    let clang = PathBuf::from(&args[2]);
    let root = PathBuf::from(&args[3]);
    if !compiler.is_file() || !clang.is_file() || !root.is_dir() {
        return Err("required native compiler, clang, or source fixtures missing".into());
    }
    let tmp = TempDir::create()?;
    let fixtures: &[(&str, Option<i32>)] = &[
        ("01-typed-default.ores", Some(7)),
        ("02-typed-const.ores", Some(7)),
        ("03-inferred-const.ores", Some(7)),
        ("04-dependent-runtime.ores", Some(21)),
        ("05-reassign.ores", None),
        ("06-self-reference.ores", None),
        ("07-type-mismatch.ores", None),
        ("08-no-initializer.ores", None),
        ("09-duplicate.ores", None),
    ];
    for (index, (name, expected)) in fixtures.iter().enumerate() {
        check(&root.join(name), &compiler, &clang, *expected, &tmp, index)?;
    }
    println!("immutable-default: 4 compiled native guests + 5 rejected admissions");
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("immutable-default runner FAILED: {error}");
        std::process::exit(1);
    }
}
