//! Cross-compilation gate: the same `.atto` fixtures, compiled for every hosted
//! Linux triple, must behave exactly like the native host build.
//!
//! For each fixture that the compiler admits for `--target=host` and that links
//! with only libc, this gate:
//!   1. builds and runs it natively (the oracle: exit code + stdout);
//!   2. emits IR with `--target=<triple>` and checks the triple was stamped;
//!   3. compiles the IR with clang for that triple, links a static musl binary
//!      with `zig cc`, runs it (directly, via binfmt/qemu-user, or a runner
//!      prefix such as `qemu-arm`), and requires identical exit code + stdout.
//! It fails if any triple ends with zero compared fixtures (no vacuous pass).
//!
//! Usage:
//!   rustc --edition=2021 -D warnings tests/native_cross_target_matrix.rs -o xmatrix
//!   ./xmatrix --compiler build/oreslang-llvmc --clang clang-21 --zig zig \
//!       --root . [--runtime-dir src/main/cpp/oreslang/runtime] [--triple T ...] [--runner TRIPLE=CMD ...]
//! Default triples: x86_64, aarch64, i686, armv7 (hosted Linux).

use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

const TRIPLES: &[(&str, &str)] = &[
    ("x86_64-unknown-linux-gnu", "x86_64-linux-musl"),
    ("aarch64-unknown-linux-gnu", "aarch64-linux-musl"),
    ("i686-unknown-linux-gnu", "x86-linux-musl"),
    ("armv7-unknown-linux-gnueabihf", "arm-linux-musleabihf"),
];
const SKIP_MARKERS: &[&str] = &["/fail/", "unsupported", "reject", "invalid", "negative"];
/// The C actor runtime that compiled actor/record programs link against
/// (same set as tests/native_kafka_scalar_execution.sh, plus records/RT).
const RUNTIME_FILES: &[&str] = &[
    "ActorOwnedI64Heap.c", "NativeActorScheduler.c", "NativeOresVM.c", "NativeActorFuture.c",
    "SourceActorBootstrap.c", "NativeExecutionContextStore.c", "NativeContinuationStore.c",
    "NativeAwaitTraceBridge.c", "NativeLogicalTrace.c", "NativeRecordRegion.c", "NativeRtDomain.c",
];

struct Options {
    compiler: String,
    clang: String,
    zig: String,
    root: PathBuf,
    triples: Vec<String>,
    runners: BTreeMap<String, Vec<String>>,
    /// When set, fixtures that need the C actor runtime are linked with it
    /// (natively for the oracle, cross-compiled per triple for targets).
    runtime_dir: Option<PathBuf>,
}

fn fail(why: &str) -> ! {
    eprintln!("FAIL native cross-target matrix: {why}");
    process::exit(1);
}

fn parse() -> Options {
    let mut o = Options {
        compiler: String::new(),
        clang: "clang".into(),
        zig: "zig".into(),
        root: PathBuf::from("."),
        triples: Vec::new(),
        runners: BTreeMap::new(),
        runtime_dir: None,
    };
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut value = || args.next().unwrap_or_else(|| fail(&format!("{arg} requires a value")));
        match arg.as_str() {
            "--compiler" => o.compiler = value(),
            "--clang" => o.clang = value(),
            "--zig" => o.zig = value(),
            "--root" => o.root = PathBuf::from(value()),
            "--triple" => o.triples.push(value()),
            "--runtime-dir" => o.runtime_dir = Some(PathBuf::from(value())),
            "--runner" => {
                let v = value();
                let (t, cmd) = v.split_once('=').unwrap_or_else(|| fail("--runner wants TRIPLE=CMD"));
                o.runners.insert(t.to_string(), cmd.split_whitespace().map(String::from).collect());
            }
            _ => fail(&format!("unknown argument: {arg}")),
        }
    }
    if o.compiler.is_empty() {
        fail("--compiler PATH required");
    }
    if o.triples.is_empty() {
        o.triples = TRIPLES.iter().map(|(t, _)| t.to_string()).collect();
    }
    o
}

/// (exit code, stdout, stderr); None if the program could not be started.
fn run(program: &str, args: &[&str]) -> Option<(i32, Vec<u8>, String)> {
    let out = Command::new(program).args(args).output().ok()?;
    let code = out.status.code().unwrap_or(-1);
    Some((code, out.stdout, String::from_utf8_lossy(&out.stderr).into_owned()))
}

fn fixtures(root: &Path) -> Vec<PathBuf> {
    let mut found = Vec::new();
    let mut stack = vec![root.join("tests/fixtures")];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else { continue };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.extension().is_some_and(|e| e == "atto") {
                let s = path.to_string_lossy();
                if !SKIP_MARKERS.iter().any(|m| s.contains(m)) {
                    found.push(path);
                }
            }
        }
    }
    found.sort();
    found
}

fn main() {
    let o = parse();
    let work = env::temp_dir().join(format!("oreslang-cross-matrix-{}", process::id()));
    fs::create_dir_all(&work).unwrap_or_else(|e| fail(&e.to_string()));
    let p = |name: &str| work.join(name).to_string_lossy().into_owned();
    let mut compared: BTreeMap<String, (usize, usize)> = o.triples.iter().map(|t| (t.clone(), (0, 0))).collect();
    let mut failures = Vec::new();
    let (mut admitted, mut host_runtime, mut runtime_linked) = (0usize, 0usize, 0usize);

    // Runtime sources and per-triple runtime objects (cross-compiled once).
    let include = format!("-I{}", o.root.join("src/main/cpp").display());
    let runtime_sources: Vec<String> = match &o.runtime_dir {
        Some(dir) => RUNTIME_FILES.iter().map(|f| dir.join(f).to_string_lossy().into_owned()).collect(),
        None => Vec::new(),
    };
    let mut runtime_objects: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for triple in &o.triples {
        let Some(zt) = TRIPLES.iter().find(|(t, _)| t == triple).map(|(_, z)| *z) else { continue };
        let mut objs = Vec::new();
        for (k, source) in runtime_sources.iter().enumerate() {
            let obj = p(&format!("rt.{triple}.{k}.o"));
            let args = ["cc", "-target", zt, "-std=c11", "-O2", "-pthread", "-D_GNU_SOURCE",
                "-DORESLANG_SOURCE_ACTOR_TESTING", "-Wall", "-Werror", include.as_str(), "-c", source.as_str(), "-o", obj.as_str()];
            match run(&o.zig, &args) {
                Some((0, _, _)) => objs.push(obj),
                other => fail(&format!("{triple}: runtime {source} did not cross-compile: {}", other.map(|r| r.2).unwrap_or_default())),
            }
        }
        runtime_objects.insert(triple.clone(), objs);
    }

    for (i, fixture) in fixtures(&o.root).iter().enumerate() {
        let src = fixture.to_string_lossy().into_owned();
        let name = fixture.strip_prefix(&o.root).unwrap_or(fixture).to_string_lossy().into_owned();
        // 1. Native oracle.
        let Some((0, host_ir, _)) = run(&o.compiler, &["--target=host", &src]) else { continue };
        if host_ir.is_empty() {
            continue;
        }
        admitted += 1;
        let (hll, hbin) = (p(&format!("{i}.host.ll")), p(&format!("{i}.host.bin")));
        fs::write(&hll, &host_ir).unwrap_or_else(|e| fail(&e.to_string()));
        let mut needs_runtime = false;
        if !matches!(run(&o.clang, &["-O2", "-x", "ir", &hll, "-o", &hbin]), Some((0, _, _))) {
            // Not libc-only: link the C actor runtime, as the host test scripts do.
            let mut args: Vec<&str> = vec!["-O2", "-x", "ir", &hll, "-x", "c"];
            args.extend(runtime_sources.iter().map(String::as_str));
            args.extend(["-std=c11", "-pthread", "-DORESLANG_SOURCE_ACTOR_TESTING", include.as_str(), "-o", hbin.as_str()]);
            if runtime_sources.is_empty() || !matches!(run(&o.clang, &args), Some((0, _, _))) {
                host_runtime += 1; // needs a runtime this gate was not given
                continue;
            }
            needs_runtime = true;
            runtime_linked += 1;
        }
        let Some((host_code, host_out, _)) = run(&hbin, &[]) else { fail(&format!("{name}: host binary did not start")) };

        for triple in &o.triples {
            let zig_target = TRIPLES.iter().find(|(t, _)| t == triple).map(|(_, z)| *z)
                .unwrap_or_else(|| fail(&format!("no zig target for {triple}")));
            let stem = format!("{i}.{triple}");
            let (ll, obj, bin) = (p(&format!("{stem}.ll")), p(&format!("{stem}.o")), p(&format!("{stem}.bin")));
            // 2. Emit for the triple.
            let target_arg = format!("--target={triple}");
            let ir = match run(&o.compiler, &[&target_arg, &src]) {
                Some((0, ir, _)) if !ir.is_empty() => ir,
                other => {
                    failures.push(format!("{triple} {name}: emission failed: {}", other.map(|r| r.2).unwrap_or_default().trim()));
                    continue;
                }
            };
            if !String::from_utf8_lossy(&ir).contains(&format!("target triple = \"{triple}\"")) {
                failures.push(format!("{triple} {name}: IR is not stamped with the target triple"));
                continue;
            }
            fs::write(&ll, &ir).unwrap_or_else(|e| fail(&e.to_string()));
            // 3. Compile, link, run, compare.
            let clang_target = format!("--target={triple}");
            if !matches!(run(&o.clang, &[&clang_target, "-O2", "-c", "-x", "ir", &ll, "-o", &obj]), Some((0, _, _))) {
                failures.push(format!("{triple} {name}: clang could not compile the IR"));
                continue;
            }
            let mut link: Vec<&str> = vec!["cc", "-target", zig_target, "-static", &obj];
            if needs_runtime {
                link.extend(runtime_objects[triple].iter().map(String::as_str));
                link.push("-pthread");
            }
            link.extend(["-o", bin.as_str()]);
            if !matches!(run(&o.zig, &link), Some((0, _, _))) {
                failures.push(format!("{triple} {name}: static link failed"));
                continue;
            }
            let result = match o.runners.get(triple) {
                Some(prefix) => {
                    let mut args: Vec<&str> = prefix[1..].iter().map(String::as_str).collect();
                    args.push(&bin);
                    run(&prefix[0], &args)
                }
                None => run(&bin, &[]),
            };
            let entry = compared.get_mut(triple).expect("triple");
            match result {
                Some((code, out, _)) if code == host_code && out == host_out => entry.0 += 1,
                Some((code, _, _)) => {
                    entry.1 += 1;
                    failures.push(format!("{triple} {name}: exit {code} / stdout differs from host exit {host_code}"));
                }
                None => failures.push(format!("{triple} {name}: target binary did not start (missing binfmt/qemu runner?)")),
            }
        }
    }
    let _ = fs::remove_dir_all(&work);
    println!("fixtures admitted for host: {admitted}; linked with the C actor runtime: {runtime_linked}; not linkable: {host_runtime}");
    for (triple, (same, diff)) in &compared {
        println!("{triple}: identical={same} mismatched={diff}");
        if *same == 0 {
            failures.push(format!("{triple}: no fixture was compared (vacuous pass refused)"));
        }
    }
    if !failures.is_empty() {
        for f in &failures {
            eprintln!("  {f}");
        }
        fail(&format!("{} problem(s)", failures.len()));
    }
    println!("PASS native cross-target matrix: {} triples", compared.len());
}
