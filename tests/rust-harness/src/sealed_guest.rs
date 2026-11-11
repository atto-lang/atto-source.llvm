//! Exact-compiler-source sealed-state guest execution test.
//! No model/simulated guest functions: compile real .atto -> LLVM IR, then link
//! compiler-defined actor entrypoints to the production native heap/scheduler.
//! Suspension, five-backend parity, and hard-RT remain separate gated claims.
use std::{
    env,
    ffi::OsString,
    fs,
    path::{Path, PathBuf},
    process::{Command, Output},
    time::{SystemTime, UNIX_EPOCH},
};

struct Workspace(PathBuf);
impl Workspace {
    fn new() -> Result<Self, String> {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|e| format!("system clock: {e}"))?
            .as_nanos();
        let dir = env::temp_dir().join(format!(
            "oreslang-sealed-guest-{}-{now}",
            std::process::id()
        ));
        fs::create_dir(&dir).map_err(|e| format!("create {}: {e}", dir.display()))?;
        Ok(Self(dir))
    }
}
impl Drop for Workspace {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[derive(Debug)]
struct Options {
    compiler: PathBuf,
    clang: PathBuf,
    root: PathBuf,
    sanitize: bool,
    auditor: PathBuf,
}
fn output(program: &Path, args: &[OsString], sanitize: bool) -> Result<Output, String> {
    let mut command = Command::new(program);
    command.args(args);
    if sanitize {
        // Darwin ASan cannot run LeakSanitizer. Keep address/UB checking
        // enabled there; Linux retains leak detection as an additional check.
        command.env(
            "ASAN_OPTIONS",
            if cfg!(target_os = "macos") {
                "detect_leaks=0:halt_on_error=1"
            } else {
                "detect_leaks=1:halt_on_error=1"
            },
        );
        command.env("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1");
    }
    let temp = crate::TempDir::new("atto-sealed-command-").map_err(|e| e.to_string())?;
    crate::retained_guest::bounded(&mut command, &temp.path.join("command"))
}
fn checked(program: &Path, args: &[OsString], sanitize: bool) -> Result<Output, String> {
    let result = output(program, args, sanitize)?;
    if !result.status.success() {
        return Err(format!(
            "{} failed {:?} stderr={} stdout={}",
            program.display(),
            result.status.code(),
            String::from_utf8_lossy(&result.stderr),
            String::from_utf8_lossy(&result.stdout)
        ));
    }
    Ok(result)
}
fn arg(value: impl Into<OsString>) -> OsString {
    value.into()
}
fn fixture(root: &Path, name: &str) -> PathBuf {
    root.join("tests").join("fixtures").join(name)
}
fn expect_target_denial(o: &Options, target: &str, kind: &str) -> Result<(), String> {
    let source = fixture(
        &o.root,
        &format!("source-isolated-actor-sealed-{kind}.atto"),
    );
    let selector = format!("--target={target}");
    let result = output(
        &o.compiler,
        &[arg(selector), arg(source.as_os_str())],
        false,
    )?;
    let diagnostic = String::from_utf8_lossy(&result.stderr);
    if result.status.success()
        || !result.stdout.is_empty()
        || !diagnostic.contains("native sealed actor-state guest v1 requires host scheduler")
    {
        return Err(format!(
            "{target}/{kind}: target did not fail closed; code={:?}; stderr={diagnostic}",
            result.status.code()
        ));
    }
    Ok(())
}
fn expect_realtime_denial(o: &Options, flag: &str, kind: &str) -> Result<(), String> {
    let source = fixture(
        &o.root,
        &format!("source-isolated-actor-sealed-{kind}.atto"),
    );
    let result = output(&o.compiler, &[arg(flag), arg(source.as_os_str())], false)?;
    let stderr = String::from_utf8_lossy(&result.stderr);
    if result.status.success()
        || !result.stdout.is_empty()
        || !stderr.contains("native sealed actor-state guest v1 lacks hard-RT execution and cancellation certification")
    {
        return Err(format!(
            "{flag}/{kind}: uncertified sealed state entered hard-RT mode; code={:?}; stderr={stderr}",
            result.status.code()
        ));
    }
    Ok(())
}
fn verify_guest_ir(ir: &str, kind: &str) -> Result<(), String> {
    if !ir.starts_with("; ModuleID") || ir.contains("gc.statepoint") || ir.contains("gc.relocate") {
        return Err("missing native LLVM module or unexpected tracing GC".into());
    }
    for symbol in [
        "oreslang_actor_Counter_init",
        "oreslang_actor_Counter_on_message",
    ] {
        if !ir
            .lines()
            .any(|line| line.starts_with("define ") && line.contains(&format!("@{symbol}(")))
        {
            return Err(format!("missing compiled LLVM definition: {symbol}"));
        }
    }
    let init = "call void @ores_actor_i64_heap_field_init(";
    let mutable_init = "call void @ores_actor_i64_heap_field_init_mutable(";
    let write = "call void @ores_actor_i64_heap_field_write(";
    if kind == "read" && (!ir.contains(init) || ir.contains(mutable_init) || ir.contains(write)) {
        return Err("critical-read generated mutable state or a write".into());
    }
    if kind == "write" && (!ir.contains(mutable_init) || !ir.contains(write)) {
        return Err("critical-write lacked mutable field initialization or write".into());
    }
    Ok(())
}
fn compile(
    o: &Options,
    ir_path: &Path,
    bridge: &Path,
    scheduler: bool,
    executable: &Path,
    optimization: &str,
) -> Result<(), String> {
    let mut args: Vec<OsString> = vec![
        arg("-std=c11"),
        arg("-pthread"),
        arg("-UNDEBUG"),
        arg("-Wall"),
        arg("-Wextra"),
        arg("-Wpedantic"),
        arg("-Werror"),
        arg("-Wno-error=override-module"),
    ];
    if o.sanitize {
        args.extend([
            arg("-g"),
            arg("-fsanitize=address,undefined"),
            arg("-fno-omit-frame-pointer"),
        ]);
    }
    args.push(arg(optimization));
    args.extend([
        arg("-I"),
        arg(o.root.join("src/main/cpp").as_os_str()),
        arg("-x"),
        arg("ir"),
        arg(ir_path.as_os_str()),
        arg("-x"),
        arg("c"),
        arg(bridge.as_os_str()),
    ]);
    if scheduler {
        args.push(arg(o
            .root
            .join("src/main/cpp/oreslang/runtime/NativeActorScheduler.c")
            .as_os_str()));
    }
    args.extend([
        arg(o
            .root
            .join("src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c")
            .as_os_str()),
        arg("-o"),
        arg(executable.as_os_str()),
    ]);
    checked(&o.clang, &args, o.sanitize)?;
    Ok(())
}
fn run_receipts(
    executable: &Path,
    kind: &str,
    scheduler: bool,
    sanitize: bool,
) -> Result<(), String> {
    let required = if scheduler {
        format!(
            "COMPILED_SEALED_SCHEDULER_{}_PASS processed=2 abandoned=0",
            kind.to_uppercase()
        )
    } else {
        format!("COMPILED_SEALED_{}_GUEST_PASS", kind.to_uppercase())
    };
    let repetitions = if sanitize { 3 } else { 12 };
    for attempt in 0..repetitions {
        let result = checked(executable, &[arg(kind)], sanitize)?;
        let receipt = String::from_utf8_lossy(&result.stdout);
        if receipt.trim() != required {
            return Err(format!(
                "{kind} scheduler={scheduler} attempt={attempt}: missing exact guest receipt: {receipt}"
            ));
        }
    }
    Ok(())
}
pub fn run_cases(
    compiler: &Path,
    clang: &Path,
    root: &Path,
    auditor: &Path,
    mode: &str,
) -> Result<(), String> {
    if !matches!(mode, "release" | "sanitize") {
        return Err("expected release or sanitize".into());
    }
    let o = Options {
        compiler: compiler.into(),
        clang: clang.into(),
        root: root.into(),
        auditor: auditor.into(),
        sanitize: mode == "sanitize",
    };
    for target in ["wasm32", "thumbv7m-none-eabi"] {
        for kind in ["read", "write"] {
            expect_target_denial(&o, target, kind)?;
        }
    }
    for flag in ["--profile=native-hard-rt", "--rt-check"] {
        for kind in ["read", "write"] {
            expect_realtime_denial(&o, flag, kind)?;
        }
    }
    let workspace = Workspace::new()?;
    for kind in ["read", "write"] {
        let source = fixture(
            &o.root,
            &format!("source-isolated-actor-sealed-{kind}-actor-only.atto"),
        );
        let result = checked(&o.compiler, &[arg(source.as_os_str())], false)?;
        let ir = String::from_utf8(result.stdout).map_err(|e| format!("non-UTF8 LLVM IR: {e}"))?;
        verify_guest_ir(&ir, kind)?;
        let ll = workspace.0.join(format!("{kind}.ll"));
        fs::write(&ll, ir).map_err(|e| format!("write LLVM IR: {e}"))?;
        checked(&o.auditor, &[arg(ll.as_os_str())], false)?;
        for scheduler in [false, true] {
            let bridge = if scheduler {
                o.root
                    .join("tests/native_isolated_sealed_state_scheduler_guest_test.c")
            } else {
                o.root
                    .join("tests/native_isolated_sealed_state_guest_bridge_test.c")
            };
            let name = if scheduler { "scheduler" } else { "heap" };
            let optimizations: &[&str] = if o.sanitize {
                &["-O1"]
            } else {
                &["-O0", "-O2"]
            };
            for optimization in optimizations {
                let executable = workspace.0.join(format!("{name}-{kind}-{optimization}"));
                compile(&o, &ll, &bridge, scheduler, &executable, optimization)?;
                run_receipts(&executable, kind, scheduler, o.sanitize)?;
            }
        }
    }
    println!(
        "COMPILED_SEALED_STATE_GUEST_RUST_PASS mode={} guests=2 scheduler=1 heap=1",
        if o.sanitize { "ASAN_UBSAN" } else { "RELEASE" }
    );
    Ok(())
}
