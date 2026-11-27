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

use std::collections::{BTreeMap, BTreeSet};
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
// Independent semantic oracle: a truncated i32 computation cannot satisfy
// the >32-bit comparisons even if it preserves the final exit-code byte.
// Use integer remainder: `/` is a float-producing operator and is a separate
// currently unsupported LLVM feature, not an integer-width oracle.
const REQUIRED_WIDE_SOURCE: &str = r#"
fnc wide_product(int left, int right): int {
  return left * right;
}
pub fnc main(): int {
  return (
    wide_product(65536, 65537) == 4295032832 &&
    wide_product(65536, 65537) % 65537 == 0 &&
    wide_product(65536, 65537) - 4294967296 == 65536 &&
    0 - wide_product(65536, 65537) < 0 - 4294967296
  ) ? 42 : 1;
}
"#;
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
                let prefix: Vec<String> = cmd.split_whitespace().map(String::from).collect();
                if prefix.is_empty() || o.runners.insert(t.to_string(), prefix).is_some() {
                    fail("empty or duplicate runner prefix");
                }
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
    let unique: BTreeSet<_> = o.triples.iter().collect();
    if unique.len() != o.triples.len() || o.triples.iter().any(|t| !TRIPLES.iter().any(|(known, _)| known == t)) {
        fail("unknown or duplicate target triple");
    }
    if o.runners.keys().any(|t| !unique.contains(t)) {
        fail("runner provided for an unselected target");
    }
    o
}

/// Reject host-fallback objects and binaries before running a target lane.
/// This is an ELF identity preflight, not a complete loader or libc ABI check.
fn elf_identity(bytes: &[u8], triple: &str, executable: bool) -> bool {
    let (class, machine, header_len) = match triple {
        "x86_64-unknown-linux-gnu" => (2, 62, 64),
        "aarch64-unknown-linux-gnu" => (2, 183, 64),
        "i686-unknown-linux-gnu" => (1, 3, 52),
        "armv7-unknown-linux-gnueabihf" => (1, 40, 52),
        _ => return false,
    };
    if bytes.len() < header_len || &bytes[..4] != b"\x7fELF" || bytes[4] != class || bytes[5] != 1 || bytes[6] != 1 {
        return false;
    }
    let kind = u16::from_le_bytes([bytes[16], bytes[17]]);
    let actual = u16::from_le_bytes([bytes[18], bytes[19]]);
    let version = u32::from_le_bytes(bytes[20..24].try_into().expect("ELF version"));
    let (flags_offset, size_offset) = if class == 1 { (36, 40) } else { (48, 52) };
    let flags = u32::from_le_bytes(bytes[flags_offset..flags_offset + 4].try_into().expect("ELF flags"));
    let declared_size = u16::from_le_bytes(bytes[size_offset..size_offset + 2].try_into().expect("ELF header size"));
    if actual != machine || version != 1 || usize::from(declared_size) != header_len {
        return false;
    }
    if machine == 40 {
        // ARM EABI5. Relocatable objects often carry no float ABI flag;
        // their CPU architecture and VFP calling convention require
        // separate .ARM.attributes checks. Linked hard-float images must
        // explicitly identify hard-float and must not claim soft-float.
        const EABI_MASK: u32 = 0xff00_0000;
        const EABI5: u32 = 0x0500_0000;
        const FLOAT_MASK: u32 = 0x600;
        const HARD_FLOAT: u32 = 0x400;
        if flags & EABI_MASK != EABI5
            || (executable && flags & FLOAT_MASK != HARD_FLOAT)
            || (!executable && !matches!(flags & FLOAT_MASK, 0 | HARD_FLOAT))
        {
            return false;
        }
    }
    if executable {
        return kind == 2 || kind == 3;
    }
    // Relocatable object structural preflight, matching the shared target
    // profiles contract. Reject host fallbacks and truncated/overflowing
    // section tables before passing an object to the linker. Extended ELF
    // section numbering is outside this bounded gate.
    if kind != 1 { return false; }
    let word = |offset: usize| -> u64 {
        if class == 1 {
            u64::from(u32::from_le_bytes(bytes[offset..offset + 4].try_into().expect("ELF32 word")))
        } else {
            u64::from_le_bytes(bytes[offset..offset + 8].try_into().expect("ELF64 word"))
        }
    };
    let half = |offset: usize| -> u16 {
        u16::from_le_bytes(bytes[offset..offset + 2].try_into().expect("ELF half"))
    };
    let (phoff, shoff, phnum, shentsize, shnum, shstrndx, section_size) =
        if class == 1 { (28, 32, 44, 46, 48, 50, 40u16) }
        else { (32, 40, 56, 58, 60, 62, 64u16) };
    let start = word(shoff);
    let count = half(shnum);
    let names = half(shstrndx);
    if word(24) != 0 || word(phoff) != 0 || half(phnum) != 0 ||
        start < header_len as u64 || count == 0 || names == 0 ||
        names >= count || half(shentsize) != section_size {
        return false;
    }
    start.checked_add(u64::from(count) * u64::from(section_size))
        .is_some_and(|end| end <= bytes.len() as u64)
}

fn require_elf(path: &str, triple: &str, executable: bool) {
    let bytes = fs::read(path).unwrap_or_else(|e| fail(&format!("{path}: {e}")));
    if !elf_identity(&bytes, triple, executable) {
        fail(&format!("{triple}: wrong ELF class, byte order, machine, type, header, section table or ABI flags: {path}"));
    }
}

/// Run deterministic negative checks on every invocation of the matrix.
fn check_identity_guard() {
    for (triple, _) in TRIPLES {
        let (class, machine, len) = match *triple {
            "x86_64-unknown-linux-gnu" => (2, 62u16, 64),
            "aarch64-unknown-linux-gnu" => (2, 183, 64),
            "i686-unknown-linux-gnu" => (1, 3, 52),
            "armv7-unknown-linux-gnueabihf" => (1, 40, 52),
            _ => fail("unhandled guard fixture"),
        };
        let section_size = if class == 1 { 40usize } else { 64usize };
        let mut h = vec![0u8; len + 2 * section_size];
        h[..4].copy_from_slice(b"\x7fELF");
        h[4] = class; h[5] = 1; h[6] = 1; h[16] = 1;
        h[18..20].copy_from_slice(&machine.to_le_bytes());
        h[20..24].copy_from_slice(&1u32.to_le_bytes());
        let size_offset = if class == 1 { 40 } else { 52 };
        h[size_offset..size_offset + 2].copy_from_slice(&(len as u16).to_le_bytes());
        let (shoff, shentsize, shnum, shstrndx) =
            if class == 1 { (32, 46, 48, 50) } else { (40, 58, 60, 62) };
        if class == 1 {
            h[shoff..shoff + 4].copy_from_slice(&(len as u32).to_le_bytes());
        } else {
            h[shoff..shoff + 8].copy_from_slice(&(len as u64).to_le_bytes());
        }
        h[shentsize..shentsize + 2].copy_from_slice(&(section_size as u16).to_le_bytes());
        h[shnum..shnum + 2].copy_from_slice(&2u16.to_le_bytes());
        h[shstrndx..shstrndx + 2].copy_from_slice(&1u16.to_le_bytes());
        if machine == 40 {
            h[36..40].copy_from_slice(&0x0500_0000u32.to_le_bytes());
        }
        assert!(elf_identity(&h, triple, false));
        assert!(!elf_identity(&h[..h.len() - 1], triple, false));
        let mut invalid = h.clone();
        let word_size = if class == 1 { 4 } else { 8 };
        invalid[shoff..shoff + word_size].fill(0xff);
        assert!(!elf_identity(&invalid, triple, false));
        for offset in [shentsize, shnum, shstrndx] {
            let mut invalid = h.clone();
            invalid[offset..offset + 2].fill(0);
            assert!(!elf_identity(&invalid, triple, false));
        }
        let mut invalid = h.clone();
        invalid[24] = 1;
        assert!(!elf_identity(&invalid, triple, false));
        let mut invalid = h.clone();
        invalid[20..24].copy_from_slice(&0u32.to_le_bytes());
        assert!(!elf_identity(&invalid, triple, false));
        let mut invalid = h.clone();
        invalid[size_offset..size_offset + 2].copy_from_slice(&0u16.to_le_bytes());
        assert!(!elf_identity(&invalid, triple, false));
        assert!(!elf_identity(&h, triple, true));
        assert!(!elf_identity(&h[..20], triple, false));
        for (other, _) in TRIPLES {
            if other != triple { assert!(!elf_identity(&h, other, false)); }
        }
        h[16] = 2;
        if machine == 40 {
            // EABI5 with no hard-float declaration is not sufficient.
            assert!(!elf_identity(&h, triple, true));
            for bad in [0x0500_0200u32, 0x0500_0600, 0x0400_0400] {
                h[36..40].copy_from_slice(&bad.to_le_bytes());
                assert!(!elf_identity(&h, triple, true));
            }
            h[36..40].copy_from_slice(&0x0500_0400u32.to_le_bytes());
        }
        assert!(elf_identity(&h, triple, true));
        assert!(!elf_identity(&h, triple, false));
        h[5] = 2;
        assert!(!elf_identity(&h, triple, true));
    }
    println!("PASS ELF identity guard positive/negative checks (host structural only)");
}

/// (exit code, stdout, stderr); None if the program could not be started.
fn run(program: &str, args: &[&str]) -> Option<(i32, Vec<u8>, String)> {
    // This hosted Linux harness requires GNU coreutils timeout. Bound every
    // compiler, linker and guest invocation, including descendant processes.
    // Reserve ambiguous timeout/launcher exit codes rather than counting a
    // matching pair of infrastructure failures as guest parity.
    let out = Command::new("timeout")
        .args(["--kill-after=5s", "--", "120s"])
        .arg(program).args(args).output()
        .unwrap_or_else(|e| fail(&format!("cannot start timeout for {program}: {e}")));
    let code = out.status.code().unwrap_or(-1);
    if code < 0 || (124..=127).contains(&code) || code >= 128 {
        fail(&format!("{program}: timeout, launcher failure, signal, or reserved exit status {code}"));
    }
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
    check_identity_guard();
    let work = env::temp_dir().join(format!("oreslang-cross-matrix-{}", process::id()));
    fs::create_dir(&work).unwrap_or_else(|e| fail(&e.to_string()));
    let required_fixture = work.join("required-wide-i64.atto");
    fs::write(&required_fixture, REQUIRED_WIDE_SOURCE).unwrap_or_else(|e| fail(&e.to_string()));
    let required_path = required_fixture.to_string_lossy();
    match run(&o.compiler, &["--target=not-a-real-target", &required_path]) {
        Some((2, out, _)) if out.is_empty() => {},
        _ => fail("unknown target did not fail with exit 2 and empty IR"),
    }
    let mut required_passed = BTreeSet::new();
    let p = |name: &str| work.join(name).to_string_lossy().into_owned();
    let mut compared: BTreeMap<String, (usize, usize)> = o.triples.iter().map(|t| (t.clone(), (0, 0))).collect();
    let mut failures = Vec::new();
    let (mut admitted, mut host_runtime, mut runtime_linked) = (0usize, 0usize, 0usize);
    let mut host_rejected = 0usize;
    let mut host_empty = 0usize;

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
                Some((0, _, _)) => { require_elf(&obj, triple, false); objs.push(obj); }
                other => fail(&format!("{triple}: runtime {source} did not cross-compile: {}", other.map(|r| r.2).unwrap_or_default())),
            }
        }
        runtime_objects.insert(triple.clone(), objs);
    }

    let mut selected = vec![required_fixture.clone()];
    selected.extend(fixtures(&o.root));
    for (i, fixture) in selected.iter().enumerate() {
        let required = fixture == &required_fixture;
        let src = fixture.to_string_lossy().into_owned();
        let name = fixture.strip_prefix(&o.root).unwrap_or(fixture).to_string_lossy().into_owned();
        // 1. Native oracle.
        let host_ir = match run(&o.compiler, &["--target=host", &src]) {
            Some((0, ir, _)) => ir,
            other => {
                let (status, _, diagnostic) = other.unwrap_or((-1, Vec::new(), String::new()));
                if required {
                    fail(&format!("mandatory wide-i64 guest rejected by host compiler (exit {status}): {diagnostic}"));
                }
                host_rejected += 1;
                println!("NOT_COMPARED host emission rejected: {name} (exit {status}): {diagnostic}");
                continue;
            }
        };
        if host_ir.is_empty() {
            if required { fail("mandatory wide-i64 guest produced empty IR"); }
            host_empty += 1;
            failures.push(format!("{name}: host compiler exited zero with empty IR"));
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
                if required { fail("mandatory wide-i64 guest did not link"); }
                host_runtime += 1;
                println!("NOT_COMPARED host link failed: {name}");
                if o.runtime_dir.is_some() {
                    failures.push(format!("{name}: host link failed despite supplied runtime"));
                }
                continue;
            }
            needs_runtime = true;
            runtime_linked += 1;
        }
        let Some((host_code, host_out, _)) = run(&hbin, &[]) else { fail(&format!("{name}: host binary did not start")) };

        if host_code < 0 { fail(&format!("{name}: host oracle terminated by signal")); }
        if required && (host_code != 42 || !host_out.is_empty()) {
            fail("mandatory wide-i64 host guest must independently return 42 with empty stdout");
        }

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
            require_elf(&obj, triple, false);
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
            require_elf(&bin, triple, true);
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
                Some((code, out, _)) if code == host_code && out == host_out => {
                    entry.0 += 1;
                    if required {
                        required_passed.insert(triple.clone());
                        println!("PASS mandatory wide-i64 guest {triple}: exit=42");
                    }
                },
                Some((code, _, _)) => {
                    entry.1 += 1;
                    failures.push(format!("{triple} {name}: exit {code} / stdout differs from host exit {host_code}"));
                }
                None => failures.push(format!("{triple} {name}: target binary did not start (missing binfmt/qemu runner?)")),
            }
        }
    }
    if o.runtime_dir.is_some() && runtime_linked == 0 {
        failures.push("runtime requested but no runtime-backed fixture linked; vacuous actor lane refused".into());
    }
    let _ = fs::remove_dir_all(&work);
    println!("fixtures admitted for host: {admitted}; linked with the C actor runtime: {runtime_linked}; not linkable: {host_runtime}; host rejected: {host_rejected}; host empty: {host_empty}");
    println!("Scope: host-admitted, successfully linked fixtures only; not a complete positive/negative corpus qualification.");
    for (triple, (same, diff)) in &compared {
        if !required_passed.contains(triple) {
            failures.push(format!("{triple}: mandatory wide-i64 guest did not execute successfully"));
        }
        let link_abi = TRIPLES.iter().find(|(t, _)| t == triple).expect("selected triple").1;
        println!("IR target={triple} link target={link_abi} libc=musl: identical={same} mismatched={diff}");
        if *same <= 1 {
            failures.push(format!("{triple}: no additional repository fixture was compared (mandatory canary alone is insufficient)"));
        }
    }
    if !failures.is_empty() {
        for f in &failures {
            eprintln!("  {f}");
        }
        fail(&format!("{} problem(s)", failures.len()));
    }
    println!("PASS scoped musl differential matrix: {} triples", compared.len());
}
