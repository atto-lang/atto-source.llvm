//! Rust-only executable LLVM source-actor / native Clang guest acceptance.
//! This intentionally ports the closed 16 positive / 49 negative fixture
//! matrix from source_actor_e2e.py without invoking Python. Legacy .ores
//! input fixture names are maintained until the compiler migration to .atto;
//! they are NOT broker implementation code or modern syntax conformance.
//!
//! No mock IR, compiler substitutions, guest execution fallback, or PASS on skip.
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[derive(Clone, Copy)]
struct Positive {
    name: &'static str,
    actors: usize,
    accepted: Option<i64>,
    last: Option<i64>,
}
const POSITIVES: &[Positive] = &[
    Positive { name: "source-actor-spawn-stop.ores", actors: 1, accepted: Some(0), last: None },
    Positive { name: "source-actor-method-keyword.ores", actors: 1, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-bare-method.ores", actors: 1, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-send.ores", actors: 1, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-typed-ref.ores", actors: 1, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-await-ready.ores", actors: 1, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-await-worker.ores", actors: 1, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-future-void-ready.ores", actors: 1, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-await-done.ores", actors: 1, accepted: Some(0), last: None },
    Positive { name: "source-actor-five-workers.ores", actors: 5, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-fifo-ten.ores", actors: 1, accepted: Some(10), last: Some(2853116705) },
    Positive { name: "source-actor-scoped-owned-i64.ores", actors: 1, accepted: Some(8), last: Some(1004) },
    Positive { name: "source-actor-two-owned-heaps.ores", actors: 2, accepted: Some(4), last: None },
    Positive { name: "source-actor-interleaved-stop.ores", actors: 2, accepted: None, last: None },
    Positive { name: "source-actor-send-moved-value.ores", actors: 1, accepted: Some(2), last: Some(3) },
    Positive { name: "source-actor-send-reborrow-then-move.ores", actors: 1, accepted: Some(2), last: Some(3) },

];
const NEGATIVES: &[(&str, &str)] = &[
    ("source-actor-owned-unknown-initializer-reject.ores", "unknown parameter 'not_yet_declared'"),
    ("source-actor-owned-forward-reference-reject.ores", "unknown parameter 'second'"),
    ("source-actor-unflagged-owned.ores", "experimental owned int requires --experimental-owned-int"),
    ("source-actor-scoped-owned-escape-reject.ores", "native actor owned temporary return requires pure scalar expression"),
    ("source-actor-static-method-reject.ores", "static method is invalid"),
    ("source-actor-typed-field-reject.ores", "native final actor fields require typed field initialization and ownership lowering"),
    ("source-actor-spawn-missing-stop.ores", "actor reference must be stopped"),
    ("source-actor-typed-ref-mismatch.ores", "ActorRef<T> type mismatch"),
    ("source-actor-typed-ref-nonspawn.ores", "ActorRef<T> requires spawn initializer"),
    ("source-future-typed-unimplemented.ores", "Future<T> requires native continuation lowering"),
    ("source-actor-await-unknown.ores", "await get_ready requires a live direct ActorRef receiver"),
    ("source-actor-await-get-done.ores", "actor effects after await get_done are unsupported"),
    ("source-actor-await-worker-twice.ores", "native source supports only one direct ActorRef lifecycle await"),
    ("source-actor-await-after-stop.ores", "await get_ready must precede post-await actor effects"),
    ("source-actor-await-non-hasfuture-scalar.ores", "await get_ready requires a live direct ActorRef receiver"),
    ("source-actor-future-void-unused.ores", "Future<void> readiness token must be awaited exactly once"),
    ("source-actor-future-void-wrong-name.ores", "native Future<void> await requires its declared readiness token"),
    ("source-actor-future-void-double-await.ores", "native source supports only one direct ActorRef lifecycle await"),
    ("source-actor-future-void-unspawned.ores", "Future<void> requires previously spawned ActorRef.get_ready"),
    ("source-actor-future-void-shadow.ores", "Future<void> readiness token shadows a live owner"),
    ("source-actor-future-void-duplicate.ores", "native source supports only one Future<void> readiness token"),
    ("source-actor-future-void-wrong-producer.ores", "expected 'get_ready'"),
    ("source-actor-future-void-send.ores", "unknown parameter"),
    ("source-actor-future-void-await-worker-instead.ores", "native Future<void> await requires its declared readiness token"),
    ("source-actor-future-void-await-again.ores", "native source supports only one direct ActorRef lifecycle await"),
    ("source-actor-spawn-double-stop.ores", "invalid or repeated actor stop"),
    ("source-actor-send-after-stop.ores", "actor message sent after stop"),
    ("source-actor-spawn-unknown.ores", "unknown actor type"),
    ("source-actor-spawn-unavailable-await.ores", "await get_ready requires one source actor without suspended locals"),
    ("source-actor-await-ready-impure-result.ores", "await get_ready requires side-effect-free scalar return"),
    ("source-actor-await-done-missing-stop.ores", "await get_done requires stop before await"),
    ("source-actor-await-done-effect-after.ores", "actor effects after await get_done are unsupported"),
    ("source-actor-await-done-impure-result.ores", "await get_done requires side-effect-free scalar return"),
    ("source-actor-await-two-lifecycle.ores", "native source supports only one direct ActorRef lifecycle await"),
    ("source-actor-await-spawn.ores", "native async/channel/select boundary unsupported"),
    ("source-actor-ref-local-shadow.ores", "scalar local shadows actor reference"),
    ("source-actor-spawn-escape.ores", "unknown parameter"),
    ("source-actor-send-missing-handler.ores", "native actor supports only on_message"),
    ("source-actor-send-unknown-ref.ores", "unknown actor message receiver"),
    ("source-actor-send-ref-as-payload.ores", "unknown parameter"),
    ("source-actor-send-after-take-reject.ores", "use of explicitly moved int"),
    ("source-actor-send-borrow-reject.ores", "borrowed reference may only be passed"),
    ("source-actor-send-return-after-take-reject.ores", "use of explicitly moved int"),
    ("source-actor-send-expression-after-take-reject.ores", "use of explicitly moved int"),
    ("source-actor-send-class-value-reject.ores", "ownership violation: class is non-Copy"),
    ("source-actor-send-moved-class-field-reject.ores", "ownership violation: use-after-move class field"),
    ("source-actor-send-field-take-reject.ores", "ownership violation: use-after-move class field"),
    ("source-actor-final-group-field-reject.ores", "native final actor fields require typed field initialization and ownership lowering"),
    ("source-actor-suspend-method-reject.ores", "native suspends actor method requires continuation lowering"),

];

struct Options {
    compiler: PathBuf,
    clang: PathBuf,
    root: PathBuf,
    sanitize: bool,
}
fn options() -> Result<Options, String> {
    let mut compiler = None;
    let mut clang = None;
    let mut root = None;
    let mut sanitize = false;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let value = match arg.as_str() {
            "--sanitize" => { if sanitize { return Err("duplicate --sanitize".into()); } sanitize = true; continue; }
            "--compiler" | "--clang" | "--root" => args.next().ok_or_else(|| format!("{arg}: missing value"))?,
            _ => return Err(format!("unknown argument: {arg}")),
        };
        match arg.as_str() {
            "--compiler" if compiler.is_none() => compiler = Some(PathBuf::from(value)),
            "--clang" if clang.is_none() => clang = Some(PathBuf::from(value)),
            "--root" if root.is_none() => root = Some(PathBuf::from(value)),
            _ => return Err(format!("duplicate argument: {arg}")),
        }
    }
    Ok(Options {
        compiler: compiler.ok_or("missing --compiler")?,
        clang: clang.ok_or("missing --clang")?,
        root: root.ok_or("missing --root")?,
        sanitize,
    })
}

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Result<Self, String> {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map_err(|e|e.to_string())?.as_nanos();
        let path = std::env::temp_dir().join(format!("atto-native-actor-rust-{}-{}", std::process::id(), nanos));
        fs::create_dir(&path).map_err(|e|format!("cannot create {}: {e}",path.display()))?;
        Ok(Self(path))
    }
    fn path(&self, name: &str) -> PathBuf { self.0.join(name) }
}
impl Drop for Scratch {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}

struct ResultRecord {
    code: Option<i32>,
    stdout: String,
    stderr: String,
}
fn execute(mut cmd: Command, scratch: &Scratch, label: &str) -> Result<ResultRecord,String> {
    let outpath = scratch.path(&format!("{label}.out"));
    let errpath = scratch.path(&format!("{label}.err"));
    let output = File::create(&outpath).map_err(|e|e.to_string())?;
    let error = File::create(&errpath).map_err(|e|e.to_string())?;
    let mut child = cmd.stdout(Stdio::from(output)).stderr(Stdio::from(error))
        .spawn().map_err(|e|format!("failed to spawn {label}: {e}"))?;
    let start = Instant::now();
    let status: ExitStatus = loop {
        match child.try_wait().map_err(|e|e.to_string())? {
            Some(status) => break status,
            None if start.elapsed() >= Duration::from_secs(90) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("{label}: command timed out at 90 seconds"));
            }
            None => thread::sleep(Duration::from_millis(5)),
        }
    };
    Ok(ResultRecord {
        code: status.code(),
        stdout: fs::read_to_string(&outpath).map_err(|e|e.to_string())?,
        stderr: fs::read_to_string(&errpath).map_err(|e|e.to_string())?,
    })
}
fn check(condition: bool, context: impl AsRef<str>) -> Result<(), String> {
    if condition { Ok(()) } else { Err(context.as_ref().to_owned()) }
}
fn count(haystack: &str, needle: &str) -> usize { haystack.matches(needle).count() }
fn exact_line<'a>(stderr: &'a str, marker: &str) -> Result<&'a str, String> {
    let mut matching = stderr.lines().filter(|s|s.contains(marker));
    let first = matching.next().ok_or_else(||format!("missing {marker}"))?;
    check(matching.next().is_none(),format!("repeated {marker}"))?;
    Ok(first)
}
fn prefix_last(name: &str, actor_index: usize, processed: i64) -> i64 {
    if processed == 0 { return 0; }
    match name {
        "source-actor-fifo-ten.ores" => (1..=processed).fold(0_i64, |a,n| a * 11 + n),
        "source-actor-scoped-owned-i64.ores" => (1..=processed).fold(0_i64, |a,n| 2*(a+n)),
        "source-actor-two-owned-heaps.ores" => {
            let first = if actor_index == 0 { 1 } else { 5 };
            (first..first+processed).fold(0_i64, |a,n| 2*(a+n))
        }
        "source-actor-interleaved-stop.ores" => {
            let values: &[i64] = if actor_index == 0 { &[1] } else { &[3,4] };
            values[..(processed as usize)].iter().sum()
        }
        _ => [1_i64,2].into_iter().take(processed as usize).sum(),
    }
}
#[derive(Debug)]
struct Receipt {
    constructor_on_worker: i64, released: i64, ready: i64,
    accepted: i64, processed: i64, abandoned: i64, last: i64,
}
fn receipt(line: &str) -> Result<Receipt,String> {
    let mut parts = line.split_whitespace();
    check(parts.next() == Some("SOURCE_ACTOR_SPAWN_STOP_VERIFIED"),format!("bad actor marker: {line}"))?;
    let mut fields = BTreeMap::new();
    for item in parts {
        let (key,value) = item.split_once('=').ok_or_else(||format!("malformed field: {item}"))?;
        check(["constructor_on_worker","released","ready","accepted","processed","abandoned","last"].contains(&key),format!("unknown field {key}"))?;
        check(key=="last" && value.starts_with('-') || value.bytes().all(|b|b.is_ascii_digit()),format!("invalid receipt number {key}={value}"))?;
        let number = value.parse::<i64>().map_err(|e|e.to_string())?;
        check(fields.insert(key,number).is_none(),format!("duplicate field {key}"))?;
    }
    check(fields.len()==7,format!("incomplete actor receipt: {line}"))?;
    let get=|key| -> i64 { *fields.get(key).expect("validated receipt fields") };
    let r=Receipt{constructor_on_worker:get("constructor_on_worker"),released:get("released"),
        ready:get("ready"),accepted:get("accepted"),processed:get("processed"),
        abandoned:get("abandoned"),last:get("last")};
    check(r.released==1 && (r.constructor_on_worker==0||r.constructor_on_worker==1) &&
        (r.ready==0||r.ready==1),format!("invalid lifecycle receipt: {r:?}"))?;
    if r.constructor_on_worker == 0 {
        check(r.ready==0 && r.processed==0 && r.last==0,format!("cancelled startup claimed execution: {r:?}"))?;
    }
    Ok(r)
}
fn negatives(options:&Options, scratch:&Scratch)->Result<(),String> {
    for (i,(name,diagnostic)) in NEGATIVES.iter().enumerate() {
        let input=options.root.join("tests/fixtures").join(name);
        check(input.is_file(),format!("negative fixture missing: {}",input.display()))?;
        let mut command=Command::new(&options.compiler);
        if name.starts_with("source-actor-scoped-owned-") || name.starts_with("source-actor-owned-") {
            command.arg("--experimental-owned-int");
        }
        let result=execute({command.arg(input);command},scratch,&format!("neg-{i}"))?;
        check(result.code != Some(0) && result.code.is_some() && result.stderr.contains(diagnostic) &&
              result.stdout.trim().is_empty(),
              format!("negative {name} emitted IR or lost expected diagnostic {diagnostic:?}: exit={:?} stderr={}",result.code,result.stderr))?;
    }
    println!("PASS {} no-IR actor ownership and await boundary cases",NEGATIVES.len());
    Ok(())
}
fn positives(options:&Options, scratch:&Scratch)->Result<(),String> {
    let runtime=options.root.join("src/main/cpp/oreslang/runtime");
    for (i,case) in POSITIVES.iter().enumerate() {
        let source=options.root.join("tests/fixtures").join(case.name);
        check(source.is_file(),format!("missing fixture: {}",source.display()))?;
        let mut compiler=Command::new(&options.compiler);
        if ["source-actor-scoped-owned-i64.ores","source-actor-two-owned-heaps.ores"].contains(&case.name) {
            compiler.arg("--experimental-owned-int");
        }
        compiler.arg(source);
        let result=execute(compiler,scratch,&format!("pos-compile-{i}"))?;
        let ir=&result.stdout;
        check(result.code==Some(0) && ir.starts_with("; ModuleID") &&
            ir.contains("oreslang_source_actor_spawn") && ir.contains("oreslang_source_actor_stop"),
            format!("{}: compiler failed or missed actor IR: exit={:?}, stderr={}",case.name,result.code,result.stderr))?;
        if !["source-actor-spawn-stop.ores","source-actor-await-done.ores"].contains(&case.name) {
            check(ir.contains("oreslang_source_actor_send"),format!("{}: missing native actor send",case.name))?;
        }
        if ["source-actor-scoped-owned-i64.ores","source-actor-two-owned-heaps.ores"].contains(&case.name) {
            check(count(ir,"call void @ores_actor_i64_heap_scoped_new(")==2 &&
                 count(ir,"call void @ores_actor_i64_heap_scoped_drop(")==2 && ir.contains("i64 3"),
                 format!("{}: missing scoped heap create/drop/reserve",case.name))?;
        }
        if ["source-actor-await-ready.ores","source-actor-await-worker.ores","source-actor-future-void-ready.ores"].contains(&case.name) {
            check(ir.contains("oreslang_source_actor_await_ready") && ir.contains("ready_continuation") &&
               ir.lines().filter(|x|x.contains("call i64 @oreslang_source_actor_await_ready(")).count()==1,
               format!("{}: missing one real await/continuation bridge",case.name))?;
        }
        if case.name=="source-actor-await-done.ores" {
            check(["oreslang_source_actor_request_stop","oreslang_source_actor_await_done",
                "done_continuation","call void @oreslang_source_actor_request_stop"].iter().all(|x|ir.contains(x)),
                "native await_done lacks real STOP/continuation")?;
        }
        if case.name=="source-actor-interleaved-stop.ores" {
            let first_stop=ir.find("call void @oreslang_source_actor_stop");
            let last_send=ir.rfind("call void @oreslang_source_actor_send");
            check(first_stop.is_some() && last_send.is_some() && first_stop<last_send,
                "native actor source effects reordered")?;
        }
        let ll=scratch.path(&format!("actor-{i}.ll"));
        let exe=scratch.path(&format!("actor-{i}.guest"));
        fs::write(&ll,ir).map_err(|e|e.to_string())?;
        let mut clang=Command::new(&options.clang);
        if options.sanitize {
            clang.args(["-O1","-g","-fno-omit-frame-pointer","-fsanitize=address,undefined"]);
        } else { clang.arg("-O1"); }
        clang.args(["-x","ir"]).arg(&ll).args(["-x","c"]);
        for name in [
            "ActorOwnedI64Heap.c","NativeActorScheduler.c","NativeOresVM.c",
            "NativeActorFuture.c","SourceActorBootstrap.c","NativeExecutionContextStore.c",
            "NativeContinuationStore.c","NativeAwaitTraceBridge.c","NativeLogicalTrace.c"
        ] { clang.arg(runtime.join(name)); }
        clang.args(["-std=c11","-pthread","-I"])
            .arg(options.root.join("src/main/cpp"))
            .arg("-DORESLANG_SOURCE_ACTOR_TESTING").arg("-o").arg(&exe);
        let compiled=execute(clang,scratch,&format!("pos-link-{i}"))?;
        check(compiled.code==Some(0),format!("{}: clang link failed: {}",case.name,compiled.stderr))?;
        let guest=execute(Command::new(&exe),scratch,&format!("pos-run-{i}"))?;
        check(guest.code==Some(43),format!("{}: wrong native guest exit {:?}: {}",case.name,guest.code,guest.stderr))?;
        check(exact_line(&guest.stderr,"SOURCE_ACTOR_VM_ROUTING_VERIFIED")?.contains("domains=4 main=1 cold_carriers=0"),
            format!("{}: VM routing not proven",case.name))?;
        if ["source-actor-await-ready.ores","source-actor-await-worker.ores","source-actor-future-void-ready.ores"].contains(&case.name) {
            check(exact_line(&guest.stderr,"SOURCE_AWAIT_READY_TRACE_PASS")?
                .contains("execution_id_persisted=1 fresh_resume=1 root_slot=63"),
                format!("{}: await-ready trace incomplete",case.name))?;
        }
        if case.name=="source-actor-await-done.ores" {
            check(exact_line(&guest.stderr,"SOURCE_AWAIT_DONE_TRACE_PASS")?
                .contains("terminal_ack=1 execution_id_persisted=1 fresh_resume=1 root_slot=63"),
                "await-done trace incomplete")?;
        }
        let lines:Vec<_>=guest.stderr.lines().filter(|x|x.contains("SOURCE_ACTOR_SPAWN_STOP_VERIFIED")).collect();
        check(lines.len()==case.actors,format!("{}: expected {} actor receipts, got {}",case.name,case.actors,lines.len()))?;
        for (actor_index,line) in lines.iter().enumerate() {
            let r=receipt(line)?;
            let accepted = if case.name=="source-actor-interleaved-stop.ores" {
                [1_i64,2][actor_index]
            } else { case.accepted.ok_or_else(||format!("missing case count: {}",case.name))? };
            check(r.accepted==accepted && (0..=accepted).contains(&r.processed) &&
                r.abandoned==accepted-r.processed && r.last==prefix_last(case.name,actor_index,r.processed),
                format!("{} actor {actor_index}: invalid accepted, abandoned or final state: {r:?}",case.name))?;
            if let Some(last) = case.last {
                // A STOP may abandon DATA; only assert full-run value if all
                // accepted mail was physically processed.
                if r.processed==accepted { check(r.last==last,format!("{} actor {actor_index}: wrong full-run last",case.name))?; }
            }
        }
        println!("PASS native source actor: {} actors {}",case.name,case.actors);
    }
    println!("PASS current-source actor lowering and physical heap reclamation");
    Ok(())
}
fn run()->Result<(),String>{
    let args=options()?;
    for file in [&args.compiler,&args.clang] {
        check(file.is_file(),format!("missing tool {}",file.display()))?;
    }
    let scratch=Scratch::new()?;
    negatives(&args,&scratch)?;
    positives(&args,&scratch)?;
    Ok(())
}
fn main(){if let Err(e)=run(){eprintln!("FAIL Rust source-actor execution gate: {e}");std::process::exit(1);}}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn inventory_exact() {
        assert_eq!(POSITIVES.len(),16);
        assert_eq!(NEGATIVES.len(),49);
        assert_eq!(POSITIVES.iter().filter(|x|x.name.ends_with(".ores")).count(),16);
    }
    #[test] fn receipt_denies_duplicate_and_missing_fields() {
        let s="SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 ready=1 accepted=2 processed=2 abandoned=0 last=3";
        assert!(receipt(s).is_ok());
        assert!(receipt(&format!("{s} last=3")).is_err());
        assert!(receipt("SOURCE_ACTOR_SPAWN_STOP_VERIFIED released=1").is_err());
    }
    #[test] fn receipt_denies_invalid_unreleased_and_fake_cancellation() {
        assert!(receipt("SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=0 released=1 ready=1 accepted=0 processed=0 abandoned=0 last=0").is_err());
        assert!(receipt("SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=0 ready=1 accepted=0 processed=0 abandoned=0 last=0").is_err());
    }
    #[test] fn expected_owned_heap_prefix_values() {
        assert_eq!(prefix_last("source-actor-fifo-ten.ores",0,10),2_853_116_705);
        assert_eq!(prefix_last("source-actor-scoped-owned-i64.ores",0,8),1004);
        assert_eq!(prefix_last("source-actor-two-owned-heaps.ores",0,4), 52);
    }
}
