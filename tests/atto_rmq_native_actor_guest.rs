//! Exact-source native guest proof, not a TCP broker or latency benchmark.
//! rustc --edition=2021 -D warnings tests/atto_rmq_native_actor_guest.rs -o /tmp/guest-check
//! /tmp/guest-check /path/to/oreslang-llvmc clang /path/to/source-checkout /tmp/work-dir
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

const BYTES: [i64; 8] = [65, 77, 81, 80, 0, 0, 9, 1];
const RUNTIME: [&str; 10] = [
    "ActorOwnedI64Heap.c",
    "NativeActorScheduler.c",
    "NativeOresVM.c",
    "NativeActorFuture.c",
    "SourceActorBootstrap.c",
    "NativeExecutionContextStore.c",
    "NativeContinuationStore.c",
    "NativeAwaitTraceBridge.c",
    "NativeLogicalTrace.c",
    "NativeOresVMNio.cpp", // intentionally NOT linked: native source actor does not own I/O
];

fn run(cmd: &mut Command) -> Output {
    let display = format!("{cmd:?}");
    let output = cmd.output().unwrap_or_else(|e| panic!("spawn {display}: {e}"));
    if !output.status.success() {
        panic!("{display} failed: code={:?}\nstdout={}\nstderr={}",
            output.status.code(), String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr));
    }
    output
}

fn field(line: &str, key: &str) -> i64 {
    let suffix = format!("{key}=");
    let token = line.split_whitespace().find_map(|part| part.strip_prefix(&suffix))
        .unwrap_or_else(|| panic!("receipt missing {key}: {line}"));
    token.parse::<i64>().unwrap_or_else(|_| panic!("invalid {key}={token}"))
}

fn assert_source(source: &str) {
    let contents = source.lines().filter(|line| !line.trim_start().starts_with("//"))
        .collect::<Vec<_>>().join("\n");
    assert!(contents.contains("define actor PrefaceActor as {"));
    assert!(contents.contains("pub fnc on_message(int byte): int"));
    assert!(contents.contains("digest = digest * 17 + byte;"));
    assert!(contents.contains("stop checker;"));
    assert_eq!(contents.matches("checker.send(").count(), BYTES.len());
    let expected = BYTES.iter().map(|x| format!("checker.send({x});")).collect::<Vec<_>>();
    let found = contents.lines().filter(|x| x.contains("checker.send("))
        .map(|x| x.trim().to_owned()).collect::<Vec<_>>();
    assert_eq!(found, expected);
    assert!(!contents.contains("network."));
    assert!(!contents.contains("read_bytes_async"));
    assert!(!contents.contains("read_buffer_async"));
}

fn main() {
    let args: Vec<_> = env::args_os().collect();
    assert_eq!(args.len(), 5, "usage: harness COMPILER CLANG SOURCE_ROOT WORK_DIR");
    let compiler = PathBuf::from(&args[1]);
    let clang = PathBuf::from(&args[2]);
    let root = PathBuf::from(&args[3]);
    let work = PathBuf::from(&args[4]);
    assert!(compiler.is_file(), "real native compiler required");
    fs::create_dir_all(&work).expect("create build dir");
    let source = root.join("tests/fixtures/atto-rmq-native-preface-actor.ores");
    let source_bytes = fs::read_to_string(&source).expect("read native actor guest");
    assert_source(&source_bytes);
    let output = run(Command::new(&compiler).arg(&source));
    let ir = String::from_utf8(output.stdout).expect("compiler must emit UTF-8 LLVM IR");
    assert!(ir.starts_with("; ModuleID"), "no real generated LLVM module");
    for required in ["oreslang_source_actor_spawn", "oreslang_source_actor_send",
                     "oreslang_source_actor_stop"] {
        assert!(ir.contains(required), "missing generated actor primitive {required}");
    }
    let ll = work.join("atto-rmq-native-preface-actor.ll");
    fs::write(&ll, ir).expect("write generated LLVM");
    let exe = work.join("atto-rmq-native-preface-actor");
    let runtime = root.join("src/main/cpp/oreslang/runtime");
    let mut link = Command::new(clang);
    link.arg("-O1").arg("-x").arg("ir").arg(&ll).arg("-x").arg("c");
    for name in RUNTIME[..RUNTIME.len() - 1].iter() {
        link.arg(runtime.join(name));
    }
    link.arg("-std=c11").arg("-pthread")
        .arg("-I").arg(root.join("src/main/cpp"))
        .arg("-DORESLANG_SOURCE_ACTOR_TESTING").arg("-o").arg(&exe);
    run(&mut link);
    let output = Command::new(&exe).output().expect("run native LLVM actor executable");
    assert_eq!(output.status.code(), Some(43),
        "incorrect native process status; stderr={}", String::from_utf8_lossy(&output.stderr));
    let stderr = String::from_utf8(output.stderr).expect("UTF-8 native receipts");
    let vm: Vec<&str> = stderr.lines().filter(|line|
        line.contains("SOURCE_ACTOR_VM_ROUTING_VERIFIED")).collect();
    assert_eq!(vm.len(), 1, "missing single native VM receipt: {stderr}");
    assert!(vm[0].contains("domains=4 main=1 cold_carriers=0"), "invalid VM domain receipt");
    let receipts: Vec<_> = stderr.lines().filter(|line|
        line.starts_with("SOURCE_ACTOR_SPAWN_STOP_VERIFIED ")).collect();
    assert_eq!(receipts.len(), 1, "expected exactly one native actor receipt: {stderr}");
    let receipt = receipts[0];
    let accepted = field(receipt, "accepted");
    let processed = field(receipt, "processed");
    let abandoned = field(receipt, "abandoned");
    assert_eq!(accepted, 8, "all eight preface bytes must be admitted");
    assert!((0..=8).contains(&processed), "invalid processed count");
    assert_eq!(accepted, processed + abandoned, "lost or duplicated actor mail");
    assert_eq!(field(receipt, "released"), 1, "exactly-once actor heap release");
    let expected_digest = BYTES.iter().take(processed as usize)
        .fold(0_i64, |state, octet| state * 17 + octet);
    assert_eq!(field(receipt, "last"), expected_digest,
        "actor FIFO, payload or checked state transition mismatch");
    println!("ATTO_RMQ_LLVM_NATIVE_ACTOR_GUEST=PASS accepted={accepted} processed={processed} abandoned={abandoned}");
    println!("ATTO_RMQ_LLVM_NATIVE_TCP_GUEST=NOT_IMPLEMENTED");
    println!("ATTO_RMQ_JAVA_LLVM_DIFF_PERFORMANCE=NOT_MEASURED");
}
