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
let receipt = |name: &str| -> u64 {
    spawned[0].split_whitespace().find_map(|field| {
        field.strip_prefix(&format!("{name}=")).map(|value| value.parse().unwrap())
    }).unwrap_or_else(|| panic!("{source}: missing {name}: {stderr}"))
};
assert!(spawned[0].contains("constructor_on_worker=1 released=1"));
// stop() may abandon queued DATA; get_ready() promises initialization,
// not completion of subsequent messages. Require an exact FIFO prefix
// and account for every admitted message instead of assuming a drain.
let processed = receipt("processed");
assert_eq!(receipt("accepted"), 2);
assert!(processed <= 2);
assert_eq!(receipt("abandoned"), 2 - processed);
assert_eq!(receipt("last"), [0, 1, 3][processed as usize]);
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
                          "source-actor-future-void-ready.ores"].iter().enumerate() {
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
            line.contains("@oreslang_source_actor_await_ready_effect(")).count();
        assert_eq!(await_calls, 1,
            "{name}: source HasFuture await must emit exactly one resume call");
        for symbol in ["main.ready.throw", "main.ready.status",
                       "oreslang_source_actor_unhandled_ready_effect"] {
            assert!(ir.contains(symbol), "{name}: missing typed effect boundary {symbol}");
        }
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

        // GNU/ELF link wrapping forces stop before constructor admission in
        // the actual compiler-produced guest. No test-only source syntax or
        // replacement compiler is involved. macOS keeps the success coverage;
        // the portable C runtime race test covers its failure projection.
        if cfg!(target_os = "linux") {
            let shim = work.join("stop-before-ready.c");
            fs::write(&shim, r#"
#include "oreslang/runtime/SourceActorBootstrap.h"
void* __real_oreslang_source_actor_spawn(OresSourceActorInit, OresSourceActorMessage, uint64_t);
int32_t __real_oreslang_source_actor_await_ready_effect(void*, OresSourceActorContinuation, uint64_t, int64_t*);
void* __wrap_oreslang_source_actor_spawn(OresSourceActorInit init, OresSourceActorMessage msg, uint64_t fields) {
    oreslang_source_actor_test_hold_initialization(1);
    return __real_oreslang_source_actor_spawn(init, msg, fields);
}
int32_t __wrap_oreslang_source_actor_await_ready_effect(void* actor, OresSourceActorContinuation next, uint64_t site, int64_t* result) {
    oreslang_source_actor_request_stop(actor);
    oreslang_source_actor_test_hold_initialization(0);
    return __real_oreslang_source_actor_await_ready_effect(actor, next, site, result);
}
"#).expect("write deterministic race shim");
            let failed_bin = work.join(format!("ready-failed-{index}"));
            // Reuse the exact compiler output, runtime sources and sanitizer
            // options; the final -o overrides the successful executable path.
            link.arg(&shim)
                .args(["-Wl,--wrap=oreslang_source_actor_spawn",
                       "-Wl,--wrap=oreslang_source_actor_await_ready_effect", "-o"])
                .arg(&failed_bin);
            run(&mut link, "link native typed readiness failure");
            let failed = Command::new(&failed_bin).output().expect("run failed readiness");
            assert_eq!(failed.status.code(), Some(1), "{name}: failure must reach OS boundary, not abort");
            let stderr = String::from_utf8(failed.stderr).expect("failure UTF8");
            assert!(stderr.contains("Unhandled throw ActorTerminated:"), "{name}: {stderr}");
            assert!(stderr.contains("outcome=3"), "{name}: {stderr}");
            assert!(stderr.contains("constructor_on_worker=0 released=1 ready=0 accepted=0 processed=0 abandoned=0"),
                "{name}: post-READY guest code ran or actor was not reclaimed: {stderr}");
            assert!(stderr.contains("active=0"), "{name}: actor leak: {stderr}");
            assert!(!stderr.contains("Sanitizer"), "{name}: sanitizer failure: {stderr}");
            println!("PASS {name} compiled ActorTerminated propagation and cleanup, sanitized={sanitize}");
        }
    }
}
