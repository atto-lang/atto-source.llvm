use std::{
    env,
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    process::{Command, Output},
    time::{SystemTime, UNIX_EPOCH},
};

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Result<Self, String> {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|e| format!("clock error: {e}"))?
            .as_nanos();
        let path = env::temp_dir().join(format!(
            "ores-actor-ref32-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir(&path)
            .map_err(|e| format!("create temp directory {}: {e}", path.display()))?;
        Ok(Self(path))
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn run(program: &Path, args: &[OsString], expected_output: Option<&Path>) -> Result<Output, String> {
    let output = Command::new(program)
        .args(args)
        .output()
        .map_err(|e| format!("execute {}: {e}", program.display()))?;
    if !output.status.success() {
        return Err(format!(
            "command failed ({}): {} {:?}\nstdout:\n{}\nstderr:\n{}",
            output.status,
            program.display(),
            args,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    if let Some(path) = expected_output {
        if !path.is_file() {
            return Err(format!(
                "compiler did not produce expected output: {}",
                path.display()
            ));
        }
    }
    Ok(output)
}

fn arg(value: impl Into<OsString>) -> OsString {
    value.into()
}

fn parse_args() -> Result<(PathBuf, PathBuf, PathBuf), String> {
    let mut producer = None;
    let mut clang = None;
    let mut root = None;
    let mut args = env::args_os().skip(1);
    while let Some(flag) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| format!("missing value for {:?}", flag))?;
        match flag.to_string_lossy().as_ref() {
            "--producer" => producer = Some(PathBuf::from(value)),
            "--clang" => clang = Some(PathBuf::from(value)),
            "--root" => root = Some(PathBuf::from(value)),
            other => return Err(format!("unknown argument: {other}")),
        }
    }
    Ok((
        producer.ok_or("missing --producer")?,
        clang.ok_or("missing --clang")?,
        root.ok_or("missing --root")?,
    ))
}

fn main() {
    if let Err(err) = real_main() {
        eprintln!("{err}");
        std::process::exit(1);
    }
}

fn real_main() -> Result<(), String> {
    let (producer, clang, root) = parse_args()?;
    let root = root
        .canonicalize()
        .map_err(|e| format!("resolve root {}: {e}", root.display()))?;
    let include = root.join("src/main/cpp");
    let runtime = include.join("oreslang/runtime/ActorArena.c");
    let driver = root.join("tests/native_actor_compressed32_llvm_driver.c");

    for path in [&producer, &clang, &runtime, &driver] {
        if !path.is_file() {
            return Err(format!(
                "missing exact-source integration input: {}",
                path.display()
            ));
        }
    }

    let temporary = TempDir::new()?;
    let llvm_file = temporary.path().join("verified-actor-compact32.ll");
    let generated = run(&producer, &[arg("--emit-ir")], None)?;
    let generated_stdout = String::from_utf8(generated.stdout)
        .map_err(|e| format!("LLVM producer stdout is not UTF-8: {e}"))?;
    if !generated_stdout.contains("define i64 @checked_actor_read")
        || !generated_stdout.contains("@ores_actor_arena_compact_publish_i32")
    {
        return Err("LLVM producer did not emit verified i32 actor MIR".to_string());
    }
    fs::write(&llvm_file, generated_stdout.as_bytes())
        .map_err(|e| format!("write {}: {e}", llvm_file.display()))?;

    let runtime_object = temporary.path().join("arena.o");
    let driver_object = temporary.path().join("driver.o");
    let llvm_object = temporary.path().join("managed-ref.o");
    let executable = temporary.path().join("actor-compact32-aot");
    let include_flag = format!("-I{}", include.display());

    run(
        &clang,
        &[
            arg("-std=c11"),
            arg("-O2"),
            arg("-Wall"),
            arg("-Wextra"),
            arg("-Wpedantic"),
            arg("-Werror"),
            arg(&include_flag),
            runtime.clone().into_os_string(),
            arg("-c"),
            arg("-o"),
            runtime_object.clone().into_os_string(),
        ],
        Some(&runtime_object),
    )?;
    run(
        &clang,
        &[
            arg("-std=c11"),
            arg("-O2"),
            arg("-Wall"),
            arg("-Wextra"),
            arg("-Wpedantic"),
            arg("-Werror"),
            arg(&include_flag),
            driver.clone().into_os_string(),
            arg("-c"),
            arg("-o"),
            driver_object.clone().into_os_string(),
        ],
        Some(&driver_object),
    )?;
    run(
        &clang,
        &[
            arg("-O2"),
            arg("-Wno-override-module"),
            llvm_file.clone().into_os_string(),
            arg("-c"),
            arg("-o"),
            llvm_object.clone().into_os_string(),
        ],
        Some(&llvm_object),
    )?;
    run(
        &clang,
        &[
            runtime_object.into_os_string(),
            driver_object.into_os_string(),
            llvm_object.into_os_string(),
            arg("-o"),
            executable.clone().into_os_string(),
        ],
        Some(&executable),
    )?;

    let executed = run(&executable, &[], None)?;
    let stdout = String::from_utf8(executed.stdout)
        .map_err(|e| format!("native executable stdout is not UTF-8: {e}"))?;
    if !stdout.contains("LLVM_MANAGED_COMPRESSED32_ACTOR_RUNTIME_PASS") {
        return Err("native compiled test did not report the expected success".to_string());
    }

    println!("PASS: verified actor compact32 LLVM i32 load/store against real ActorArena.c");
    Ok(())
}
