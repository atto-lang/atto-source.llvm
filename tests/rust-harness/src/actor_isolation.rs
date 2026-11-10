//! Actual .atto -> LLVM -> Clang -> native scheduler acceptance driver.
use crate::{require, run, stderr, stdout, TempDir};
use std::{
    fs,
    path::Path,
    process::{Command, Output, Stdio},
    thread,
    time::{Duration, Instant},
};

fn execute_bounded(command: &mut Command, prefix: &Path) -> Result<Output, String> {
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
        if start.elapsed() >= Duration::from_secs(30) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!(
                "native execution timed out: {:?}\n{}",
                command,
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

pub fn run_cases(compiler: &Path, clang: &Path, root: &Path) -> Result<(), String> {
    let temp = TempDir::new("atto-actor-isolation-").map_err(|e| e.to_string())?;
    let fixture = root.join("tests/fixtures/atto-actor-scalar-nontail-overflow.atto");
    let compiled = run(compiler, [fixture.as_os_str()])?;
    require(compiled.status.success(), stderr(&compiled))?;
    let ir = stdout(&compiled);
    let entry = "define i32 @main()";
    require(ir.matches(entry).count() == 1, "expected one OS entrypoint")?;
    require(
        ir.contains("call i64 @oreslang_source_actor_scalar_vm_execute")
            || ir.contains("call i32 @oreslang_source_actor_scalar_vm_execute"),
        "missing actual actor-owned iterative frame lowering",
    )?;
    // Rename only the OS entry. Actor code and constant bytecode are unchanged.
    let ir = ir.replacen(entry, "define i32 @atto_unused_guest_main()", 1);
    let ll = temp.path.join("guest.ll");
    fs::write(&ll, ir).map_err(|e| e.to_string())?;
    let runtime = root.join("src/main/cpp/oreslang/runtime");
    for (name, flags) in [
        ("O0", vec!["-O0"]),
        ("O2", vec!["-O2"]),
        (
            "sanitized",
            vec![
                "-O1",
                "-g",
                "-fno-omit-frame-pointer",
                "-fsanitize=address,undefined",
            ],
        ),
    ] {
        let binary = temp.path.join(name);
        let mut command = Command::new(clang);
        command
            .args(flags)
            .args(["-pthread", "-DORESLANG_SOURCE_ACTOR_TESTING"])
            .arg(format!("-I{}", root.join("src/main/cpp").display()))
            .arg(&ll)
            .arg(root.join("tests/source_actor_compiled_isolation_harness.c"));
        for file in [
            "ActorOwnedI64Heap.c",
            "NativeActorScheduler.c",
            "NativeOresVM.c",
            "NativeActorFuture.c",
            "SourceActorBootstrap.c",
            "NativeExecutionContextStore.c",
            "NativeContinuationStore.c",
            "NativeAwaitTraceBridge.c",
            "NativeLogicalTrace.c",
        ] {
            command.arg(runtime.join(file));
        }
        let linked = command
            .arg("-o")
            .arg(&binary)
            .output()
            .map_err(|e| e.to_string())?;
        require(
            linked.status.success(),
            format!("{name}: {}", stderr(&linked)),
        )?;
        // Low-stack native runs prove the admitted 100000-deep request is
        // stopped by the logical frame quota before a recursive native fault.
        // ASan uses its normal stack to avoid confusing its own instrumentation.
        let mut execution = if name == "sanitized" {
            let mut command = Command::new(&binary);
            command
                .env("ASAN_OPTIONS", "halt_on_error=1")
                .env("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1");
            command
        } else {
            let mut command = Command::new("bash");
            command
                .args(["-c", "ulimit -s 512; exec \"$1\"", "atto-isolation"])
                .arg(&binary);
            command
        };
        let output = execute_bounded(&mut execution, &temp.path.join(name))?;
        require(
            output.status.success(),
            format!("{name}: {:?}\n{}", output.status, stderr(&output)),
        )?;
        require(stdout(&output).trim() == "COMPILED_ACTOR_ISOLATION_PASS actors=3 frame_bytes=65536 sibling=9 replacement=11",
            format!("{name}: missing compiled execution witness"))?;
        let diagnostics = stderr(&output);
        let vm: Vec<_> = diagnostics
            .lines()
            .filter(|line| line.starts_with("SOURCE_ACTOR_SCALAR_VM_VERIFIED "))
            .collect();
        require(
            vm.len() == 3,
            format!("{name}: missing per-actor VM receipts: {diagnostics}"),
        )?;
        require(
            vm[0].contains("peak_frames=64 ") && vm[0].ends_with("error=1"),
            format!("{name}: missing actor-local frame-quota fault: {}", vm[0]),
        )?;
        require(
            vm[1..].iter().all(|line| line.ends_with("error=0")),
            format!("{name}: failure leaked to sibling or replacement"),
        )?;
        let stopped: Vec<_> = diagnostics
            .lines()
            .filter(|line| line.starts_with("SOURCE_ACTOR_SPAWN_STOP_VERIFIED "))
            .collect();
        require(
            stopped.len() == 3
                && stopped.iter().all(|line| {
                    let fields: Vec<_> = line.split_whitespace().collect();
                    fields.contains(&"released=1") && fields.contains(&"abandoned=0")
                }),
            format!("{name}: missing physical actor stop receipts"),
        )?;
        println!("PASS compiled actor isolation {name}: nonoverlapping native frame arenas, quota fault, sibling and replacement");
    }
    Ok(())
}
