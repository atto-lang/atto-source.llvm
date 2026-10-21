use std::{
    env,
    ffi::OsStr,
    fs,
    io,
    path::{Path, PathBuf},
    process::{Command, Output},
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(prefix: &str) -> io::Result<Self> {
        let counter = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        let name = format!("{prefix}{}-{nanos}-{counter}", std::process::id());
        let path = env::temp_dir().join(name);
        fs::create_dir(&path)?;
        Ok(Self { path })
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn run<I, S>(program: &Path, args: I) -> Result<Output, String>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    Command::new(program)
        .args(args)
        .output()
        .map_err(|e| format!("failed to run {}: {e}", program.display()))
}

fn stdout(out: &Output) -> String {
    String::from_utf8_lossy(&out.stdout).into_owned()
}

fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

fn require(condition: bool, message: impl Into<String>) -> Result<(), String> {
    if condition {
        Ok(())
    } else {
        Err(message.into())
    }
}

fn json_value<'a>(text: &'a str, key: &str) -> Option<&'a str> {
    let needle = format!("\"{key}\"");
    let start = text.find(&needle)? + needle.len();
    let rest = text.get(start..)?;
    let colon = rest.find(':')?;
    Some(rest.get(colon + 1..)?.trim_start())
}

fn json_bool(text: &str, key: &str) -> Option<bool> {
    let value = json_value(text, key)?;
    if value.starts_with("true") {
        Some(true)
    } else if value.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

fn json_u64(text: &str, key: &str) -> Option<u64> {
    let value = json_value(text, key)?;
    let digits: String = value.chars().take_while(|c| c.is_ascii_digit()).collect();
    digits.parse().ok()
}

fn json_string(text: &str, key: &str) -> Option<String> {
    let value = json_value(text, key)?;
    let quoted = value.strip_prefix('"')?;
    let end = quoted.find('"')?;
    Some(quoted[..end].to_string())
}

#[cfg(unix)]
fn symlink_file(src: &Path, dst: &Path) -> io::Result<()> {
    std::os::unix::fs::symlink(src, dst)
}

#[cfg(windows)]
fn symlink_file(src: &Path, dst: &Path) -> io::Result<()> {
    std::os::windows::fs::symlink_file(src, dst)
}

fn realtime_profile(compiler: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-rt-profile-").map_err(|e| e.to_string())?;
    let root = &temp.path;

    let pure = root.join("pure.ores");
    fs::write(
        &pure,
        "fnc helper(): int { return 41; }\nfnc main(): int { return helper(); }\n",
    )
    .map_err(|e| e.to_string())?;

    let out = run(compiler, [OsStr::new("--rt-check"), pure.as_os_str()])?;
    require(out.status.success(), stderr(&out))?;
    let report = stdout(&out);
    require(json_bool(&report, "source_admitted") == Some(true), "pure source must admit")?;
    require(
        json_bool(&report, "artifact_certified") == Some(false),
        "compiler check must not certify linked artifact",
    )?;
    require(
        json_u64(&report, "reachable_functions") == Some(2),
        "transitive reachable count",
    )?;

    let report_path = root.join("report.json");
    let report_flag = format!("--rt-report={}", report_path.display());
    let out = run(
        compiler,
        [
            OsStr::new("--profile=native-hard-rt"),
            OsStr::new(&report_flag),
            pure.as_os_str(),
        ],
    )?;
    require(out.status.success(), stderr(&out))?;
    require(stdout(&out).contains("define"), "admitted profile must emit LLVM IR")?;
    let persisted = fs::read_to_string(&report_path).map_err(|e| e.to_string())?;
    require(
        json_string(&persisted, "diagnostic").as_deref() == Some("OK"),
        "persisted report",
    )?;

    let io_source = root.join("io.ores");
    fs::write(
        &io_source,
        "fnc io_capable(): int using io { return 1; }\nfnc main(): int { return io_capable(); }\n",
    )
    .map_err(|e| e.to_string())?;
    let out = run(compiler, [OsStr::new("--rt-check"), io_source.as_os_str()])?;
    require(out.status.code() == Some(1), "declared IO must reject")?;
    let report = stdout(&out);
    require(
        json_bool(&report, "source_admitted") == Some(false),
        "IO report admission",
    )?;
    require(
        json_string(&report, "diagnostic").as_deref() != Some("OK"),
        "IO diagnostic",
    )?;

    let recursive = root.join("recursive.ores");
    fs::write(
        &recursive,
        "fnc bounce(): int { return bounce(); }\nfnc main(): int { return bounce(); }\n",
    )
    .map_err(|e| e.to_string())?;
    let out = run(compiler, [OsStr::new("--rt-check"), recursive.as_os_str()])?;
    require(out.status.code() == Some(1), "recursive graph must reject")?;
    require(
        json_string(&stdout(&out), "diagnostic").as_deref() == Some("RT008_UNBOUNDED_LOOP"),
        "recursion diagnostic",
    )?;

    let out = run(compiler, [OsStr::new("--rt-entry=helper/0"), pure.as_os_str()])?;
    require(
        out.status.code() == Some(2),
        "rt-entry without an RT analysis mode must reject",
    )?;

    let original = fs::read_to_string(&pure).map_err(|e| e.to_string())?;
    let alias = root.join("pure-report-alias.ores");
    symlink_file(&pure, &alias).map_err(|e| format!("create symlink: {e}"))?;
    let alias_flag = format!("--rt-report={}", alias.display());
    let out = run(
        compiler,
        [OsStr::new("--rt-check"), OsStr::new(&alias_flag), pure.as_os_str()],
    )?;
    require(
        out.status.code() == Some(1),
        "report symlink aliasing source must reject",
    )?;
    require(
        fs::read_to_string(&pure).map_err(|e| e.to_string())? == original,
        "source must not be overwritten through report symlink",
    )?;

    let hard_alias = root.join("pure-report-hardlink.ores");
    fs::hard_link(&pure, &hard_alias).map_err(|e| format!("create hard link: {e}"))?;
    let hard_flag = format!("--rt-report={}", hard_alias.display());
    let out = run(
        compiler,
        [OsStr::new("--rt-check"), OsStr::new(&hard_flag), pure.as_os_str()],
    )?;
    require(
        out.status.code() == Some(1),
        "report hardlink aliasing source must reject",
    )?;
    require(
        fs::read_to_string(&pure).map_err(|e| e.to_string())? == original,
        "source must not be overwritten through report hardlink",
    )?;

    println!("native hard-RT profile CLI: PASS");
    Ok(())
}

fn callable_nominal(compiler: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-callable-nominal-").map_err(|e| e.to_string())?;
    let source = temp.path.join("boundary.ores");
    let mut negatives: Vec<(String, Option<&str>)> = Vec::new();

    for name in ["Callable", "Fnc", "Function", "Routine", "Method"] {
        negatives.push((format!("define class {name} as end\n"), Some("nominal class types")));
    }
    negatives.push((
        "define actor Callable as\n  let count: int = 0;\n  pub fnc on_message(int value): int { return value; }\nend\n".to_string(),
        Some("nominal actor types"),
    ));
    for clause in [
        "extends Callable",
        "extends Fnc<int, int>",
        "implements Callable",
        "implements Fnc<int, int>",
    ] {
        negatives.push((format!("define class Child {clause} as end\n"), None));
    }

    let entry = "pub fnc main(): int { return 0; }\n";
    for (body, diagnostic) in &negatives {
        fs::write(&source, format!("{body}{entry}")).map_err(|e| e.to_string())?;
        let out = run(compiler, [source.as_os_str()])?;
        require(
            !out.status.success() && stdout(&out).trim().is_empty(),
            format!(
                "unsupported callable nominal form admitted: {body:?}; status={:?}; stdout={}",
                out.status.code(),
                stdout(&out)
            ),
        )?;
        if let Some(diagnostic) = diagnostic {
            require(
                stderr(&out).contains(diagnostic),
                format!("missing native diagnostic for {body:?}: {}", stderr(&out)),
            )?;
        }
    }

    fs::write(
        &source,
        "define class Ordinary as\n  pub let int value = 1;\nend\npub fnc main(): int { return 0; }\n",
    )
    .map_err(|e| e.to_string())?;
    let out = run(compiler, [source.as_os_str()])?;
    require(
        out.status.success()
            && stderr(&out).is_empty()
            && stdout(&out).starts_with("; ModuleID"),
        format!(
            "ordinary class affected: stderr={} stdout={}",
            stderr(&out),
            stdout(&out)
        ),
    )?;

    println!(
        "PASS {} negative callable-nominal tests and ordinary class",
        negatives.len()
    );
    Ok(())
}

fn aot_driver(driver: &Path, compiler: &Path, clang: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-aot-").map_err(|e| e.to_string())?;
    let source = temp.path.join("main.ores");
    let output = temp.path.join("native app");

    fs::write(&source, "fnc main(): int { return 42; }\n").map_err(|e| e.to_string())?;
    let status = Command::new(driver)
        .arg(&source)
        .arg(&output)
        .env("ORESLANG_LLVMC", compiler)
        .env("CLANG", clang)
        .status()
        .map_err(|e| format!("run AOT driver: {e}"))?;
    require(status.success(), format!("AOT driver failed: {status}"))?;

    let status = Command::new(&output)
        .env_clear()
        .env("PATH", "/nonexistent")
        .status()
        .map_err(|e| format!("run native output: {e}"))?;
    require(
        status.code() == Some(42),
        format!("native output returned {:?}", status.code()),
    )?;

    let original = fs::read(&output).map_err(|e| e.to_string())?;
    for text in [
        "import module spin from \"spin\";\n",
        "this is not Oreslang\n",
        "fnc helper(): int { return 42; }\n",
    ] {
        fs::write(&source, text).map_err(|e| e.to_string())?;
        let out = Command::new(driver)
            .arg(&source)
            .arg(&output)
            .env("ORESLANG_LLVMC", compiler)
            .env("CLANG", clang)
            .output()
            .map_err(|e| format!("run rejected AOT case: {e}"))?;
        require(!out.status.success(), format!("invalid source admitted: {text:?}"))?;
        require(
            fs::read(&output).map_err(|e| e.to_string())? == original,
            "failed compile changed existing native output",
        )?;
    }

    for entry in fs::read_dir(&temp.path).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if entry
            .file_name()
            .to_string_lossy()
            .starts_with(".ores-aot.")
        {
            return Err(format!(
                "AOT staging file leaked: {}",
                entry.path().display()
            ));
        }
    }

    println!("native AOT driver: PASS");
    Ok(())
}


fn named_call_source_order(compiler: &Path, fixture: &Path) -> Result<(), String> {
    let out = run(compiler, [fixture.as_os_str()])?;
    require(out.status.success(), format!("compile named-call fixture: {}", stderr(&out)))?;
    let ir = stdout(&out);
    require(ir.starts_with("; ModuleID"), "expected native LLVM module header")?;
    let second = ir.find("call i64 @read_second(")
        .ok_or_else(|| "missing call to read_second".to_string())?;
    let first = ir.find("call i64 @read_first(")
        .ok_or_else(|| "missing call to read_first".to_string())?;
    require(second < first, "named arguments evaluated in formal rather than source order")?;
    require(stderr(&out).is_empty(), format!("unexpected compiler stderr: {}", stderr(&out)))?;
    println!("named-call source-order IR: PASS");
    Ok(())
}

fn actor_persistent_fields(compiler: &Path, clang: &Path, root: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-actor-fields-").map_err(|e| e.to_string())?;
    let fixture = root.join("tests/fixtures/actor-owned-fields.ores");
    let compiled = run(compiler, [fixture.as_os_str()])?;
    require(compiled.status.success(), format!("compile actor fields: {}", stderr(&compiled)))?;
    require(!compiled.stdout.is_empty(), "compiler returned no actor LLVM IR")?;
    let ir = temp.path.join("actor.ll");
    let binary = temp.path.join("actor");
    fs::write(&ir, &compiled.stdout).map_err(|e| e.to_string())?;
    let linked = Command::new(clang)
        .arg(&ir)
        .arg(root.join("tests/native_actor_source_harness.c"))
        .arg(root.join("src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"))
        .arg(format!("-I{}", root.join("src/main/cpp").display()))
        .arg("-o")
        .arg(&binary)
        .output()
        .map_err(|e| format!("run clang: {e}"))?;
    require(linked.status.success(), format!("link actor fields: {}", stderr(&linked)))?;
    let executed = run(&binary, std::iter::empty::<&OsStr>())?;
    require(executed.status.success(),
        format!("actor owned-field executable exited {:?}: {}", executed.status.code(), stderr(&executed)))?;
    println!("actor persistent fields: PASS");
    Ok(())
}

fn tagged_message_wire(binary: &Path, vectors: &Path) -> Result<(), String> {
    let contents = fs::read_to_string(vectors)
        .map_err(|e| format!("read {}: {e}", vectors.display()))?;
    require(json_string(&contents, "schema").as_deref() == Some("oreslang.tagged.message.v2"),
        "tagged-message vector schema mismatch")?;
    let expected = json_string(&contents, "hex")
        .ok_or_else(|| "missing hex field in tagged-message vector".to_string())?;
    require(!expected.is_empty() && expected.bytes().all(|b| b.is_ascii_hexdigit()) && expected.len() % 2 == 0,
        "tagged-message vector hex must be nonempty even-length hexadecimal")?;
    let actual = run(binary, std::iter::empty::<&OsStr>())?;
    require(actual.status.success(), format!("tagged-message executable failed: {}", stderr(&actual)))?;
    require(stdout(&actual).trim() == expected,
        format!("native enum message bytes differ from Java fixture: {} != {expected}", stdout(&actual).trim()))?;
    println!("PASS exact enum tag/identity typed MessagePack v2 vector");
    Ok(())
}


fn native_async_io_boundary(compiler: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-async-io-boundary-").map_err(|e| e.to_string())?;
    let fixture = temp.path.join("probe.ores");
    // These names must remain rejected until typed effects, native continuation
    // lowering, and nonblocking transports are implemented together.
    let operations = [
        ("read blocking", "read_text_blocking(1)"),
        ("read async", "read_text_async(1)"),
        ("write blocking", "write_text_blocking(1)"),
        ("write async", "write_text_async(1)"),
        ("TCP connect blocking", "connect_blocking(1)"),
        ("TCP connect async", "connect_async(1)"),
        ("TCP read blocking", "read_line_blocking(1)"),
        ("TCP read async", "read_line_async(1)"),
        ("TCP write blocking", "socket_write_text_blocking(1)"),
        ("TCP write async", "socket_write_text_async(1)"),
        ("HTTP GET blocking", "http_get_text_blocking(1)"),
        ("HTTP GET async", "http_get_text_async(1)"),
        ("HTTP POST blocking", "http_post_text_blocking(1)"),
        ("HTTP POST async", "http_post_text_async(1)"),
        ("stdio blocking", "println_blocking(1)"),
        ("stdio async", "println_async(1)"),
    ];
    for (name, operation) in operations {
        fs::write(&fixture, format!("pub fnc main(): int {{ return {operation}; }}\n"))
            .map_err(|e| format!("{name}: write fixture: {e}"))?;
        let out = run(compiler, [fixture.as_os_str()])?;
        require(!out.status.success(),
            format!("{name}: unsupported native I/O compiled: {}", stdout(&out)))?;
        require(stdout(&out).trim().is_empty(),
            format!("{name}: unsupported native I/O emitted LLVM IR: {}", stdout(&out)))?;
        require(stderr(&out).contains("unknown function or wrong call arity"),
            format!("{name}: missing explicit rejection diagnostic: {}", stderr(&out)))?;
    }
    println!("Native async-only I/O boundary: {} unsupported native bindings fail closed", operations.len());
    Ok(())
}

fn path_arg(args: &[String], index: usize, usage: &str) -> Result<PathBuf, String> {
    args.get(index)
        .map(PathBuf::from)
        .ok_or_else(|| usage.to_string())
}

fn real_main() -> Result<(), String> {
    let args: Vec<String> = env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("realtime-profile") => {
            let compiler = path_arg(&args, 2, "usage: realtime-profile <compiler>")?;
            realtime_profile(&compiler)
        }
        Some("callable-nominal") => {
            let compiler = path_arg(&args, 2, "usage: callable-nominal <compiler>")?;
            callable_nominal(&compiler)
        }
        Some("native-async-io-boundary") => {
            let compiler = path_arg(&args, 2, "usage: native-async-io-boundary <compiler>")?;
            native_async_io_boundary(&compiler)
        }
        Some("named-call-order") => {
            let compiler = path_arg(&args, 2, "usage: named-call-order <compiler> <fixture>")?;
            let fixture = path_arg(&args, 3, "usage: named-call-order <compiler> <fixture>")?;
            named_call_source_order(&compiler, &fixture)
        }
        Some("actor-persistent-fields") => {
            let compiler = path_arg(&args, 2, "usage: actor-persistent-fields <compiler> <clang> <root>")?;
            let clang = path_arg(&args, 3, "usage: actor-persistent-fields <compiler> <clang> <root>")?;
            let root = path_arg(&args, 4, "usage: actor-persistent-fields <compiler> <clang> <root>")?;
            actor_persistent_fields(&compiler, &clang, &root)
        }
        Some("tagged-message-wire") => {
            let binary = path_arg(&args, 2, "usage: tagged-message-wire <binary> <vectors>")?;
            let vectors = path_arg(&args, 3, "usage: tagged-message-wire <binary> <vectors>")?;
            tagged_message_wire(&binary, &vectors)
        }
        Some("aot-driver") => {
            let driver = path_arg(&args, 2, "usage: aot-driver <driver> <compiler> <clang>")?;
            let compiler = path_arg(&args, 3, "usage: aot-driver <driver> <compiler> <clang>")?;
            let clang = path_arg(&args, 4, "usage: aot-driver <driver> <compiler> <clang>")?;
            aot_driver(&driver, &compiler, &clang)
        }
        _ => Err(
            "usage: oreslang-llvm-test-harness <realtime-profile|callable-nominal|aot-driver|named-call-order|actor-persistent-fields|tagged-message-wire|native-async-io-boundary> ..."
                .to_string(),
        ),
    }
}

fn main() {
    if let Err(error) = real_main() {
        eprintln!("FAIL: {error}");
        std::process::exit(1);
    }
}
