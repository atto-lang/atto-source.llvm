//! Real .atto -> oreslang-llvmc -> LLVM IR -> Clang -> native actor guest.
 //! Uses only the Rust standard library. This is guest execution evidence
 //! only when the target binary actually executes on an admitted runner.
use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::{Command, Output};
use std::time::{SystemTime, UNIX_EPOCH};

struct Scratch(PathBuf);
impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn run(mut cmd: Command, label: &str) -> Output {
    let output = cmd.output().unwrap_or_else(|e| panic!("{label}: spawn: {e}"));
    assert!(
        output.status.success(),
        "{label}: exit={:?}\nstdout:\n{}\nstderr:\n{}",
        output.status.code(),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    output
}

fn assert_ir(ir: &str) {
    for necessary in [
        "oreslang_source_actor_spawn_checked",
        "ores_actor_i64_heap_try_field_read",
        "ores_actor_i64_heap_try_field_write",
        "call i32 @ores_actor_i64_heap_try_field_read",
        "call i32 @ores_actor_i64_heap_try_field_write",
        "on_message_checked",
        ".read.fault",
        ".write.fault",
    ] {
        assert!(ir.contains(necessary), "checked guest LLVM IR lacks {necessary}");
    }
    assert!(
        !ir.contains("call i64 @ores_actor_i64_heap_field_read(")
            && !ir.contains("call void @ores_actor_i64_heap_field_write("),
        "source actor handler still emits fatal field access calls"
    );
}

fn arg(key: &str) -> String {
    let args: Vec<String> = env::args().collect();
    args.windows(2)
        .find(|pair| pair[0] == key)
        .map(|pair| pair[1].clone())
        .unwrap_or_else(|| panic!("missing {key}"))
}

fn main() {
    let root = PathBuf::from(arg("--root"));
    let compiler = PathBuf::from(arg("--compiler"));
    let clang = PathBuf::from(arg("--clang"));
    let fixture = root.join("tests/fixtures/source-actor-checked-field.atto");
    let runtime = root.join("src/main/cpp/oreslang/runtime");
    assert!(fixture.is_file(), "missing reviewed .atto fixture");
    assert!(compiler.is_file(), "missing exact compiler binary");

    let now = SystemTime::now().duration_since(UNIX_EPOCH)
        .expect("system clock before UNIX_EPOCH").as_nanos();
    let work = env::temp_dir().join(format!(
        "atto-checked-actor-{}-{now}", std::process::id()));
    fs::create_dir(&work).expect("create private temporary guest directory");
    let _scratch = Scratch(work.clone());

    let mut compile = Command::new(&compiler);
    compile.arg(&fixture);
    let generated = run(compile, "oreslang-llvmc compiled .atto");
    let ir = String::from_utf8(generated.stdout)
        .expect("compiler must emit UTF-8 LLVM IR");
    assert!(ir.starts_with("; ModuleID"), "compiler did not emit LLVM IR");
    assert_ir(&ir);
    let guest_ir = work.join("checked-actor.ll");
    fs::write(&guest_ir, ir).expect("write LLVM IR");
    let sources: Vec<PathBuf> = [
        "ActorOwnedI64Heap.c",
        "NativeActorScheduler.c",
        "NativeOresVM.c",
        "NativeActorFuture.c",
        "SourceActorBootstrap.c",
        "NativeExecutionContextStore.c",
        "NativeContinuationStore.c",
        "NativeAwaitTraceBridge.c",
        "NativeLogicalTrace.c",
    ].iter().map(|name| runtime.join(name)).collect();

    for optimization in ["-O0", "-O2"] {
        let guest = work.join(format!("guest-{}", &optimization[1..]));
        let mut link = Command::new(&clang);
        link.arg(optimization)
            .arg("-x").arg("ir").arg(&guest_ir)
            .arg("-x").arg("c");
        for source in &sources {
            assert!(source.is_file(), "missing reviewed native runtime: {}",
                    source.display());
            link.arg(source);
        }
        link.arg("-std=c11")
            .arg("-pthread")
            .arg("-I").arg(root.join("src/main/cpp"))
            .arg("-DORESLANG_SOURCE_ACTOR_TESTING")
            .arg("-o").arg(&guest);
        run(link, "Clang source actor link");

        let output = Command::new(&guest)
            .output().expect("spawn compiled AttoLang native actor");
        assert_eq!(
            output.status.code(), Some(43),
            "native guest {} returned {:?}; stdout={} stderr={}",
            optimization, output.status.code(),
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr));
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(
            stderr.contains("SOURCE_ACTOR_SPAWN_STOP_VERIFIED")
                && stderr.contains("accepted=2")
                && stderr.contains("processed=2"),
            "native actor did not execute and account two guest messages: {stderr}"
        );
        println!("ATTO_CHECKED_ACTOR_GUEST_{}_PASS",
                 if optimization == "-O0" { "O0" } else { "O2" });
    }
    println!("ATTO_CHECKED_ACTOR_GUEST_IR_AND_NATIVE_PASS");
    println!("SUPERVISOR_TYPED_FAILURE_DELIVERY_NOT_CERTIFIED");
}
