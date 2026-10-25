// No Python: compile three actual Oreslang actor-readiness programs to LLVM IR,
// link with the native actor scheduler, and compare physical/runtime receipts.
// Source/compiler must be exact-head, independently pinned by the CI workflow.
use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::{Command, Output};
use std::time::{SystemTime, UNIX_EPOCH};

struct Scratch(PathBuf);
impl Drop for Scratch {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}
fn run(cmd: &mut Command, stage: &str) -> Output {
    let output = cmd.output().unwrap_or_else(|err| panic!("{stage}: {err}"));
    assert!(output.status.success(),
        "{stage}: code={:?}; stdout={}; stderr={}",
        output.status.code(),
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr));
    output
}
fn expected_trace(stderr: &str, source: &str) {
    let lines: Vec<&str> = stderr.lines().collect();
    let marker = lines.iter().filter(|x| x.contains("SOURCE_AWAIT_READY_TRACE_PASS"))
        .collect::<Vec<_>>();
    assert_eq!(marker.len(), 1, "{source}: missing/duplicate await trace: {stderr}");
    assert!(marker[0].contains("execution_id_persisted=1 fresh_resume=1 root_slot=63"),
        "{source}: wrong continuation ancestry: {stderr}");
    let spawned = lines.iter().filter(|x| x.contains("SOURCE_ACTOR_SPAWN_STOP_VERIFIED"))
        .collect::<Vec<_>>();
    assert_eq!(spawned.len(), 1, "{source}: wrong actor count: {stderr}");
    assert!(spawned[0].contains("constructor_on_worker=1 released=1")
         && spawned[0].contains("processed=2")
         && spawned[0].contains("last=3"),
        "{source}: wrong actor effects: {stderr}");
    let routed = lines.iter().filter(|x| x.contains("SOURCE_ACTOR_VM_ROUTING_VERIFIED"))
        .collect::<Vec<_>>();
    assert_eq!(routed.len(), 1, "{source}: VM route count: {stderr}");
    assert!(routed[0].contains("domains=4 main=1 cold_carriers=0"),
        "{source}: eager carrier or incorrect VM route: {stderr}");
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let option = |key: &str| -> String {
        let p = args.iter().position(|s| s == key)
            .unwrap_or_else(|| panic!("missing argument {key}"));
        args.get(p + 1).unwrap_or_else(|| panic!("missing value {key}")).clone()
    };
    let compiler = option("--compiler");
    let clang = option("--clang");
    let root = PathBuf::from(option("--root"));
    let sanitize = args.iter().any(|s| s == "--sanitize");
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH)
        .expect("clock").as_nanos();
    let work = env::temp_dir().join(format!(
        "oreslang-ready-future-{}-{nanos}", std::process::id()));
    fs::create_dir(&work).expect("create private test workspace");
    let _cleanup = Scratch(work.clone());
    let runtime = root.join("src/main/cpp/oreslang/runtime");
    let includes = root.join("src/main/cpp");

    for (index, name) in ["source-actor-await-ready.ores",
                          "source-actor-await-worker.ores",
                          "source-actor-await-worker-twice.ores",
                          "source-actor-await-get-ready-twice.ores",
                          "source-actor-future-void-ready.ores",
                          "source-actor-future-void-double-await.ores",
                          "source-actor-future-void-await-again.ores",
                          "source-actor-future-void-await-worker-instead.ores"].iter().enumerate() {
        let fixture = root.join("tests/fixtures").join(name);
        let source = run(Command::new(&compiler).arg(&fixture), "compile guest");
        let ir = String::from_utf8(source.stdout).expect("LLVM IR must be UTF-8");
        assert!(ir.starts_with("; ModuleID"), "{name}: no native LLVM module");
        for symbol in ["ready_continuation", "oreslang_source_actor_await_ready",
                       "oreslang_source_actor_spawn", "oreslang_source_actor_send",
                       "oreslang_source_actor_stop"] {
            assert!(ir.contains(symbol), "{name}: missing {symbol}");
        }
        // The runtime declaration alone is not proof of suspension. Require
        // precisely one compiler-generated call to the CONTROL await bridge.
        let await_calls = ir.lines().filter(|line|
            line.contains("call ") &&
            line.contains("@oreslang_source_actor_await_ready(")).count();
        assert_eq!(await_calls, 1,
            "{name}: source HasFuture await must emit exactly one resume call");
        let ll = work.join(format!("ready-{index}.ll"));
        fs::write(&ll, &ir).expect("write LLVM IR");
        let bin = work.join(format!("ready-{index}"));
        let mut link = Command::new(&clang);
        if sanitize {
            link.args(["-O1", "-g", "-fno-omit-frame-pointer",
                       "-fsanitize=address,undefined"]);
        } else {
            link.arg("-O1");
        }
        link.args(["-x", "ir"]).arg(&ll).args(["-x", "c"]);
        for module in [
            "ActorOwnedI64Heap.c", "NativeActorScheduler.c", "NativeOresVM.c",
            "NativeActorFuture.c", "SourceActorBootstrap.c",
            "NativeExecutionContextStore.c", "NativeContinuationStore.c",
            "NativeAwaitTraceBridge.c", "NativeLogicalTrace.c",
        ] {
            link.arg(runtime.join(module));
        }
        link.args(["-std=c11", "-pthread", "-I"])
            .arg(&includes)
            .args(["-DORESLANG_SOURCE_ACTOR_TESTING", "-o"])
            .arg(&bin);
        run(&mut link, "link native scheduler AOT");
        for iteration in 0..4 {
            let output = Command::new(&bin).output()
                .unwrap_or_else(|err| panic!("{name} run {iteration}: {err}"));
            assert_eq!(output.status.code(), Some(43),
                "{name} iteration {iteration}: status={:?}; stderr={}",
                output.status.code(), String::from_utf8_lossy(&output.stderr));
            assert!(output.stdout.is_empty(), "{name}: unexpected stdout");
            let stderr = String::from_utf8(output.stderr).expect("valid trace UTF8");
            expected_trace(&stderr, name);
        }
        println!("PASS {name} actual native AOT, repeated 4 times, sanitized={sanitize}");
    }
}
