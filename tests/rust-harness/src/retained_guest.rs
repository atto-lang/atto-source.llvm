//! Real compiled guest segments under native retained DATA/WAKE. The host
//! owns continuation state; this is not source actor await lowering.
use crate::{require, stderr, stdout, TempDir};
use std::{
    fs,
    path::Path,
    process::{Command, Output, Stdio},
    thread,
    time::{Duration, Instant},
};

fn bounded(command: &mut Command, prefix: &Path) -> Result<Output, String> {
    let out_path = prefix.with_extension("stdout");
    let err_path = prefix.with_extension("stderr");
    let out_file = fs::File::create(&out_path).map_err(|e| e.to_string())?;
    let err_file = fs::File::create(&err_path).map_err(|e| e.to_string())?;
    let mut child = command
        .stdout(Stdio::from(out_file))
        .stderr(Stdio::from(err_file))
        .spawn()
        .map_err(|e| e.to_string())?;
    let start = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait().map_err(|e| e.to_string())? {
            break status;
        }
        if start.elapsed() >= Duration::from_secs(90) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!(
                "retained guest command timed out: {command:?}\n{}",
                fs::read_to_string(&err_path).unwrap_or_default()
            ));
        }
        thread::sleep(Duration::from_millis(5));
    };
    Ok(Output {
        status,
        stdout: fs::read(out_path).map_err(|e| e.to_string())?,
        stderr: fs::read(err_path).map_err(|e| e.to_string())?,
    })
}

fn checked(command: &mut Command, prefix: &Path) -> Result<Output, String> {
    let output = bounded(command, prefix)?;
    require(
        output.status.success(),
        format!(
            "{command:?}: {:?}\n{}\n{}",
            output.status,
            stdout(&output),
            stderr(&output)
        ),
    )?;
    Ok(output)
}

pub fn run_cases(
    compiler: &Path,
    clang: &Path,
    root: &Path,
    auditor: &Path,
    mode: &str,
) -> Result<(), String> {
    require(
        matches!(mode, "release" | "sanitize"),
        "expected release or sanitize",
    )?;
    let temp = TempDir::new("atto-retained-guest-").map_err(|e| e.to_string())?;
    let rejected = root.join("tests/fixtures/source-actor-suspend-method-reject.ores");
    let negative = bounded(
        Command::new(compiler).arg(rejected),
        &temp.path.join("negative"),
    )?;
    require(
        negative.status.code() == Some(1)
            && negative.stdout.is_empty()
            && stderr(&negative)
                .contains("native suspends actor method requires continuation lowering"),
        format!(
            "source suspension boundary changed: {:?}\n{}\n{}",
            negative.status,
            stdout(&negative),
            stderr(&negative)
        ),
    )?;
    println!("FULL_SOURCE_ACTOR_SUSPENDS_LOWERING_STILL_REJECTED");

    let source = root.join("tests/fixtures/native-retained-guest-steps.atto");
    let compiled = checked(
        Command::new(compiler).arg(source),
        &temp.path.join("compile"),
    )?;
    let ir = stdout(&compiled);
    for symbol in ["guest_step", "guest_resume", "guest_cleanup"] {
        require(
            ir.lines()
                .any(|line| line.starts_with(&format!("define i64 @{symbol}("))),
            format!("missing compiled guest function {symbol}"),
        )?;
    }
    let main = "define i32 @main()";
    require(
        ir.matches(main).count() == 1 && ir.matches("@main(").count() == 1,
        "expected exactly one current native OS entrypoint",
    )?;
    let ll = temp.path.join("guest.ll");
    fs::write(&ll, &ir).map_err(|e| e.to_string())?;
    checked(Command::new(auditor).arg(&ll), &temp.path.join("no-gc"))?;
    // Only rename the native OS entry; guest bodies/calls stay unchanged.
    fs::write(
        &ll,
        ir.replacen(main, "define i32 @ores_guest_fixture_main()", 1),
    )
    .map_err(|e| e.to_string())?;
    let levels: &[&str] = if mode == "sanitize" {
        &["1"]
    } else {
        &["0", "2"]
    };
    let mut runs = 0;
    for level in levels {
        let binary = temp.path.join(format!("native-O{level}"));
        let mut command = Command::new(clang);
        command
            .args([
                "-std=c11",
                "-pthread",
                "-UNDEBUG",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-Wno-error=override-module",
                "-DORESLANG_NATIVE_SCHED_VALIDATE=1",
            ])
            .arg(format!("-O{level}"));
        if mode == "sanitize" {
            command.args([
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
            ]);
        }
        command
            .arg("-I")
            .arg(root.join("src/main/cpp"))
            .args(["-x", "ir"])
            .arg(&ll)
            .args(["-x", "c"])
            .arg(root.join("tests/native_actor_compiled_guest_ledger_bridge_test.c"))
            .arg(root.join("src/main/cpp/oreslang/runtime/NativeActorScheduler.c"))
            .arg(root.join("src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"))
            .arg("-o")
            .arg(&binary);
        checked(&mut command, &temp.path.join(format!("link-{level}")))?;
        let repeat = if mode == "sanitize" { 3 } else { 6 };
        for round in 0..repeat {
            let mut execute = Command::new(&binary);
            if mode == "sanitize" {
                execute
                    .env("ASAN_OPTIONS", "halt_on_error=1")
                    .env("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1");
            }
            let result = checked(
                &mut execute,
                &temp.path.join(format!("run-{level}-{round}")),
            )?;
            require(
                stdout(&result).trim() == "LLVM_COMPILED_ATTO_SEGMENTS_RETAINED_LEDGER_PASS",
                format!("missing actual guest receipt: {}", stdout(&result)),
            )?;
            runs += 1;
        }
    }
    println!("LLVM_COMPILED_GUEST_RETAINED_SCHEDULER_PASS mode={mode} runs={runs}");
    Ok(())
}
