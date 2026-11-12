//! Production LLVM compiler -> Clang -> real C scheduler + actor heap.
//! This is an ordered four-operation-per-mail Redis scalar batch proof. NOT a
//! source-compiled RESP2/Bytes/TCP server, so do not market it as one.
use std::{env, fs};
use std::path::PathBuf;
use std::process::{Command, Output};

const COMMANDS: [i64; 3] = [1353, 3435, 2343];
const RESPONSES: [i64; 3] = [107207401207, 207301200401, 109209301200];
const RUNTIME: [&str; 9] = [
    "ActorOwnedI64Heap.c",
    "NativeActorScheduler.c",
    "NativeOresVM.c",
    "NativeActorFuture.c",
    "SourceActorBootstrap.c",
    "NativeExecutionContextStore.c",
    "NativeContinuationStore.c",
    "NativeAwaitTraceBridge.c",
    "NativeLogicalTrace.c",
];

fn run_ok(command: &mut Command) -> Output {
    let display = format!("{command:?}");
    let result = command.output().unwrap_or_else(|e| panic!("spawn {display}: {e}"));
    assert!(result.status.success(),
        "{display}: status={:?}\nstdout={}\nstderr={}",
        result.status.code(),
        String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr));
    result
}
fn field(receipt: &str, name: &str) -> i64 {
    let prefix = format!("{name}=");
    let token = receipt.split_whitespace()
        .find_map(|part| part.strip_prefix(&prefix))
        .unwrap_or_else(|| panic!("missing {name} in {receipt}"));
    token.parse().unwrap_or_else(|_| panic!("invalid {name} in {receipt}"))
}
fn verify_guest_source(source: &str) {
    assert!(source.contains("define actor NativeBatchedScalarRedis as {"));
    assert!(source.contains("let stored: int = 0;"));
    assert!(source.contains("let present: int = 0;"));
    assert!(source.contains("let digest: int = 0;"));
    assert!(source.contains("digit = (remaining >= 5000) ? 5"));
    assert!(source.contains("remaining = batch;"));
    assert!(source.contains("let digit: int = 0;"));
    assert!(source.contains("digest = digest * 1000"));
    assert!(source.contains("pub fnc on_message(int batch): int"));
    assert!(source.contains("await worker.get_ready();"));
    assert!(source.contains("stop worker;"));
    for line in ["return 207;", "return 209;"] {
        assert!(!source.contains(line),
            "GET results must derive from owned state, not fixed oracle literals");
    }
    let sent: Vec<i64> = source.lines().filter_map(|s| {
        let s = s.trim();
        s.strip_prefix("worker.send(")
            .and_then(|tail| tail.strip_suffix(");"))
            .map(|n| n.parse::<i64>().expect("source actor must send numeric command"))
    }).collect();
    assert_eq!(sent, COMMANDS, "native guest request order changed");
    for forbidden in ["network.", "read_resp2_binary_async", "write_resp2_binary_async",
                      "Map<Bytes", "native_call", "extern ", "stdio."] {
        assert!(!source.contains(forbidden), "scalar proof cannot fake unsupported {forbidden}");
    }
}
fn main() {
    let args: Vec<_> = env::args_os().collect();
    assert_eq!(args.len(), 5, "usage: guest-check COMPILER CLANG REPO_ROOT WORK_DIR");
    let compiler = PathBuf::from(&args[1]);
    let clang = PathBuf::from(&args[2]);
    let root = PathBuf::from(&args[3]);
    let work = PathBuf::from(&args[4]);
    assert!(compiler.is_file(), "compiler must be real production oreslang-llvmc");
    fs::create_dir_all(&work).expect("work directory");
    let fixture = root.join("tests/fixtures/atto-redis-native-batched-scalar-actor.ores");
    let program = fs::read_to_string(&fixture).expect("native source fixture");
    verify_guest_source(&program);
    let generated = run_ok(Command::new(&compiler).arg(&fixture));
    let ir = String::from_utf8(generated.stdout).expect("LLVM IR should be UTF-8");
    assert!(ir.starts_with("; ModuleID"), "real compiler emitted no LLVM IR");
    for symbol in ["oreslang_source_actor_spawn", "oreslang_source_actor_send",
                   "oreslang_source_actor_stop", "oreslang_source_actor_await_ready"] {
        assert!(ir.contains(symbol), "compiler missing actor lowering {symbol}");
    }
    let ir_file = work.join("atto-redis-native-batched-scalar-actor.ll");
    fs::write(&ir_file, ir).expect("write compiled LLVM IR");
    let executable = work.join("atto-redis-native-batched-scalar-actor");
    let runtime = root.join("src/main/cpp/oreslang/runtime");
    let mut build = Command::new(clang);
    build.arg("-O1").arg("-x").arg("ir").arg(&ir_file).arg("-x").arg("c");
    for file in RUNTIME { build.arg(runtime.join(file)); }
    build.arg("-std=c11").arg("-pthread")
        .arg("-I").arg(root.join("src/main/cpp"))
        .arg("-DORESLANG_SOURCE_ACTOR_TESTING")
        .arg("-o").arg(&executable);
    run_ok(&mut build);
    let executed = Command::new(&executable).output().expect("run native actor binary");
    assert_eq!(executed.status.code(), Some(43),
        "compiled native guest exit mismatch: stderr={}",
        String::from_utf8_lossy(&executed.stderr));
    let receipts = String::from_utf8(executed.stderr).expect("actor receipts UTF-8");
    let vm: Vec<_> = receipts.lines()
        .filter(|line| line.starts_with("SOURCE_ACTOR_VM_ROUTING_VERIFIED")).collect();
    assert_eq!(vm.len(), 1, "missing exact native VM routing receipt: {receipts}");
    assert!(vm[0].contains("domains=4 main=1 cold_carriers=0"));
    let actor: Vec<_> = receipts.lines()
        .filter(|line| line.starts_with("SOURCE_ACTOR_SPAWN_STOP_VERIFIED ")).collect();
    assert_eq!(actor.len(), 1, "missing unique actor terminal receipt: {receipts}");
    let receipt = actor[0];
    let accepted = field(receipt, "accepted");
    let processed = field(receipt, "processed");
    let abandoned = field(receipt, "abandoned");
    assert_eq!(accepted, COMMANDS.len() as i64, "native mailbox must admit all three bounded batches");
    assert_eq!(processed, COMMANDS.len() as i64,
        "native compiled batch guest must EXECUTE every admitted batch");
    assert_eq!(abandoned, 0, "all batch mails must physically drain before STOP");
    assert_eq!(accepted, processed + abandoned,
        "native actor must not lose or duplicate mailbox admission");
    assert_eq!(field(receipt, "released"), 1, "actor heap must retire exactly once");
    assert_eq!(field(receipt, "ready"), 1, "await get_ready must establish readiness");
    let expected_last = RESPONSES[RESPONSES.len() - 1];
    assert_eq!(field(receipt, "last"), expected_last,
        "compiled actor state machine must produce the correct FIFO-prefix reply");
    // Reject production Java-style source instead of silently lowering a
    // fake synchronous socket/host data path. This is a parity *gap* receipt.
    let unsupported = root.join("tests/fixtures/atto-redis-native-binary-full-parity.atto");
    let source = fs::read_to_string(&unsupported).expect("real canonical Redis Atto guest");
    assert!(source.contains("Map<Bytes,Bytes>"));
    assert!(source.contains("serve_resp2_async"));
    let rejected = Command::new(&compiler).arg(&unsupported)
        .output().expect("run production compiler negative gate");
    assert!(!rejected.status.success(), "unsupported full Redis source compiled silently");
    assert!(rejected.stdout.is_empty(), "unsupported native Redis emitted partial IR");
    // Pin and test the *actual* 100-client batched Atto Redis guest.
    // It must not silently degrade to this scalar-only actor compiler path.
    const CANONICAL_100_GIT_BLOB: &str = "5b6474bba468eea50a6f3f1a8dd81186ee977172";
    let canonical_100 = root.join("tests/fixtures/atto-redis-native-binary-100-batch-parity.atto");
    let canonical_text = fs::read_to_string(&canonical_100)
        .expect("exact canonical 100-client batched Redis guest");
    assert!(canonical_text.contains("read_resp2_binary_batch_async(16)"));
    assert!(canonical_text.contains("Map<Bytes,Bytes>"));
    assert!(canonical_text.contains("worker.outputs.next()"));
    assert!(canonical_text.contains("reply_100"));
    // The caller's exact-source CI independently checks this Git blob.
    let rejected_100 = Command::new(&compiler).arg(&canonical_100)
        .output().expect("run canonical 100-client guest fail-closed gate");
    assert!(!rejected_100.status.success(),
        "native compiler silently accepted 100-client typed Bytes/TCP source");
    assert!(rejected_100.stdout.is_empty(),
        "unsupported native 100-client Redis emitted partial LLVM IR");
    println!("ATTO_REDIS_LLVM_CANONICAL_100_BLOB={CANONICAL_100_GIT_BLOB}");
    println!("ATTO_REDIS_LLVM_CANONICAL_100_FULL_TCP=FAIL_CLOSED");
    println!("ATTO_REDIS_LLVM_COMPILER_NATIVE_BATCH_ACTOR=EXECUTED_PASS");
    println!("ATTO_REDIS_LLVM_NATIVE_BATCH_ACCEPTED={accepted} PROCESSED={processed} ABANDONED={abandoned}");
    println!("ATTO_REDIS_LLVM_BATCH_LOGICAL_COMMANDS=12 MAILBOX_MESSAGES=3");
    println!("ATTO_REDIS_LLVM_NATIVE_BINARY_TCP=NOT_IMPLEMENTED");
}
