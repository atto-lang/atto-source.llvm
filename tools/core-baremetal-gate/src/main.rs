//! Bare-metal `atto.core` gate.
//!
//! Compiles an `atto.core`-only AttoLang `.atto` program for `thumbv7m-none-eabi`,
//! links it with a tiny platform startup into a static image that has no
//! libc, compiler-rt, allocator, RTOS or actor runtime, audits the result,
//! and boots it on QEMU's Cortex-M3 board. It also proves the checks are not
//! vacuous: hosted imports must be rejected by the compiler, and a
//! deliberately hosted object must fail both the symbol audit and the link.

mod audit;

use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const TARGET: &str = "thumbv7m-none-eabi";
const QEMU_TIMEOUT: Duration = Duration::from_secs(30);
const PASS_LINE: &str = "ORES_CORE_BAREMETAL_PASS vectors=6 value=42";
const TRAP_LINE: &str = "ORES_CORE_TRAP hardfault";
const GUEST_EXPORTS: &[&str] = &["ores_core_entry", "ores_core_overflow_probe"];
const LINKER_SCRIPT_SYMBOLS: &[&str] = &[
    "__stack_top", "__data_load", "__data_start", "__data_end", "__bss_start", "__bss_end",
];

struct Tools {
    compiler: PathBuf,
    clang: PathBuf,
    ld_lld: PathBuf,
    llvm_nm: PathBuf,
    llvm_readelf: PathBuf,
    qemu: PathBuf,
}

struct Gate {
    tools: Tools,
    fixtures: PathBuf,
    work: PathBuf,
    checks: usize,
}

type GateResult<T> = Result<T, String>;

struct Output {
    status: Option<i32>,
    stdout: String,
    stderr: String,
}

impl Output {
    /// QEMU sends semihosting console output to stderr; match on both streams.
    fn console(&self) -> String {
        format!("{}{}", self.stdout, self.stderr)
    }
}

fn main() -> ExitCode {
    match parse_args().and_then(run) {
        Ok(checks) => {
            println!("core bare-metal gate PASS ({checks} checks)");
            ExitCode::SUCCESS
        }
        Err(message) => {
            eprintln!("core bare-metal gate FAIL: {message}");
            ExitCode::FAILURE
        }
    }
}

fn parse_args() -> GateResult<(Tools, PathBuf, Option<PathBuf>)> {
    let mut compiler = None;
    let mut tools = Tools {
        compiler: PathBuf::new(),
        clang: "clang".into(),
        ld_lld: "ld.lld".into(),
        llvm_nm: "llvm-nm".into(),
        llvm_readelf: "llvm-readelf".into(),
        qemu: "qemu-system-arm".into(),
    };
    let mut fixtures =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures/core_baremetal");
    let mut keep_work = None;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let (key, value) = match arg.split_once('=') {
            Some((k, v)) => (k.to_string(), v.to_string()),
            None => {
                let v = args.next().ok_or_else(|| format!("missing value for {arg}"))?;
                (arg, v)
            }
        };
        let value = PathBuf::from(value);
        match key.as_str() {
            "--compiler" => compiler = Some(value),
            "--clang" => tools.clang = value,
            "--ld-lld" => tools.ld_lld = value,
            "--llvm-nm" => tools.llvm_nm = value,
            "--llvm-readelf" => tools.llvm_readelf = value,
            "--qemu" => tools.qemu = value,
            "--fixtures" => fixtures = value,
            "--keep-work" => keep_work = Some(value),
            other => return Err(format!("unknown option {other}")),
        }
    }
    tools.compiler = compiler.ok_or("--compiler <path to oreslang-llvmc> is required")?;
    Ok((tools, fixtures, keep_work))
}

fn run((tools, fixtures, keep_work): (Tools, PathBuf, Option<PathBuf>)) -> GateResult<usize> {
    let work = match &keep_work {
        Some(dir) => dir.clone(),
        None => {
            let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_nanos());
            std::env::temp_dir().join(format!("ores-core-baremetal-{}-{nanos}", std::process::id()))
        }
    };
    std::fs::create_dir_all(&work).map_err(|e| format!("create {}: {e}", work.display()))?;
    let mut gate = Gate { tools, fixtures, work: work.clone(), checks: 0 };
    let result = gate.run_all();
    if keep_work.is_none() {
        let _ = std::fs::remove_dir_all(&work);
    }
    result.map(|()| gate.checks)
}

impl Gate {
    fn run_all(&mut self) -> GateResult<()> {
        self.report_toolchain()?;
        let guest = self.compile_guest()?;
        let startup = self.compile_startup("startup.o", &[])?;
        let startup_trap = self.compile_startup("startup_trap.o", &["-DORES_PROBE_TRAP"])?;
        let image = self.link_and_audit("app.elf", &startup, &guest)?;
        let trap_image = self.link_and_audit("trap.elf", &startup_trap, &guest)?;
        self.boot_pass(&image)?;
        self.boot_trap(&trap_image)?;
        self.compiler_rejections()?;
        self.auditor_self_test(&startup)?;
        Ok(())
    }

    fn pass(&mut self, what: &str) {
        self.checks += 1;
        println!("PASS {what}");
    }

    fn report_toolchain(&mut self) -> GateResult<()> {
        for (name, tool) in [
            ("clang", &self.tools.clang),
            ("ld.lld", &self.tools.ld_lld),
            ("qemu", &self.tools.qemu),
        ] {
            let out = exec(Command::new(tool).arg("--version"), None)?;
            let first = out.stdout.lines().next().unwrap_or("").trim();
            println!("TOOL {name}: {first}");
        }
        Ok(())
    }

    /// Oreslang source -> LLVM IR -> ARM object, with IR and object audits.
    fn compile_guest(&mut self) -> GateResult<PathBuf> {
        let source = self.fixtures.join("app.atto");
        let out = exec(
            Command::new(&self.tools.compiler)
                .arg(format!("--target={TARGET}"))
                .arg("--library-profile=core")
                .arg(&source),
            None,
        )?;
        require(out.status == Some(0), || format!("atto.core guest failed to compile:\n{}", out.stderr))?;
        let ir = out.stdout;
        require(ir.contains(&format!("target triple = \"{TARGET}\"")), || {
            "guest IR lacks the thumbv7m-none-eabi triple".into()
        })?;
        require(!ir.contains("@main("), || "guest IR defines a hosted main entry point".into())?;
        let externals = audit::ir_external_declarations(&ir);
        require(externals.is_empty(), || {
            format!("atto.core guest IR declares external functions: {externals:?}")
        })?;
        let hatches = audit::ir_escape_hatches(&ir);
        require(hatches.is_empty(), || {
            format!("atto.core guest IR reaches the host without a declaration: {hatches:?}")
        })?;
        self.pass("atto.core .atto guest compiles to IR with no external declarations, globals or assembly");

        // thumbv7m-none-eabi must default to the core profile: compiling without
        // the flag has to produce the same module, so the default cannot drift.
        let default = exec(
            Command::new(&self.tools.compiler).arg(format!("--target={TARGET}")).arg(&source),
            None,
        )?;
        require(default.status == Some(0) && default.stdout == ir, || {
            "thumbv7m-none-eabi without --library-profile no longer emits the core-profile module".into()
        })?;
        self.pass("thumbv7m-none-eabi defaults to the core profile");

        let ir_path = self.work.join("guest.ll");
        write(&ir_path, &ir)?;
        let object = self.work.join("guest.o");
        let mut cmd = self.clang_arm();
        cmd.arg("-Wno-override-module").arg("-c").arg(&ir_path).arg("-o").arg(&object);
        require_ok(exec(&mut cmd, None)?, "clang guest object")?;

        let symbols = self.nm(&object)?;
        let violations = audit::audit_guest_object(&symbols);
        require(violations.is_empty(), || violations.join("\n"))?;
        for export in GUEST_EXPORTS {
            require(symbols.iter().any(|s| s.kind == 'T' && s.name == *export), || {
                format!("guest object does not define `{export}`")
            })?;
        }
        self.pass("guest object requests nothing outside the freestanding support ABI");
        Ok(object)
    }

    fn compile_startup(&mut self, name: &str, defines: &[&str]) -> GateResult<PathBuf> {
        let object = self.work.join(name);
        let mut cmd = self.clang_arm();
        cmd.args(["-ffreestanding", "-nostdlib", "-fno-builtin", "-Wall", "-Wextra", "-Werror"])
            .args(defines)
            .arg("-c")
            .arg(self.fixtures.join("startup.c"))
            .arg("-o")
            .arg(&object);
        require_ok(exec(&mut cmd, None)?, "clang platform startup")?;
        // The platform shim is held to the same rules as the guest, apart from
        // the guest entry points and linker-script symbols it legitimately needs.
        let undefined: Vec<_> = self
            .nm(&object)?
            .into_iter()
            .filter(|s| {
                !GUEST_EXPORTS.contains(&s.name.as_str())
                    && !LINKER_SCRIPT_SYMBOLS.contains(&s.name.as_str())
            })
            .collect();
        let violations = audit::audit_guest_object(&undefined);
        require(violations.is_empty(), || format!("platform startup {name}: {}", violations.join("\n")))?;
        self.pass(&format!("platform startup {name} is freestanding"));
        Ok(object)
    }

    fn link(&self, name: &str, objects: &[&Path]) -> GateResult<(PathBuf, Output)> {
        let image = self.work.join(name);
        let mut cmd = Command::new(&self.tools.ld_lld);
        cmd.arg("-T").arg(self.fixtures.join("lm3s6965.ld")).arg("--gc-sections");
        cmd.args(objects).arg("-o").arg(&image);
        Ok((image, exec(&mut cmd, None)?))
    }

    fn link_and_audit(&mut self, name: &str, startup: &Path, guest: &Path) -> GateResult<PathBuf> {
        let (image, out) = self.link(name, &[startup, guest])?;
        require_ok(out, &format!("ld.lld {name} (no libc, no compiler-rt)"))?;
        let mut violations = audit::audit_linked_image(&self.nm(&image)?);
        let headers = exec(Command::new(&self.tools.llvm_readelf).arg("-lW").arg(&image), None)?;
        violations.extend(audit::audit_program_headers(&headers.stdout));
        let sections = exec(Command::new(&self.tools.llvm_readelf).arg("-SW").arg(&image), None)?;
        violations.extend(audit::audit_sections(&sections.stdout));
        require(violations.is_empty(), || format!("{name}: {}", violations.join("\n")))?;
        self.pass(&format!("{name} links with no libc and has no loader, TLS or constructor state"));
        Ok(image)
    }

    fn qemu(&self, image: &Path) -> GateResult<Output> {
        exec(
            Command::new(&self.tools.qemu)
                .args(["-M", "lm3s6965evb", "-cpu", "cortex-m3", "-nographic"])
                .args(["-monitor", "none", "-serial", "none"])
                .args(["-semihosting-config", "enable=on,target=native"])
                .arg("-kernel")
                .arg(image),
            Some(QEMU_TIMEOUT),
        )
    }

    fn boot_pass(&mut self, image: &Path) -> GateResult<()> {
        let out = self.qemu(image)?;
        let console = out.console();
        let unexpected = ["ORES_CORE_TRAP", "ORES_CORE_BAREMETAL_FAIL", "ORES_CORE_UNEXPECTED_EXCEPTION"]
            .iter()
            .any(|marker| console.contains(marker));
        require(out.status == Some(0) && console.contains(PASS_LINE) && !unexpected, || {
            format!("bare-metal run did not pass (status {:?}):\n{}{}", out.status, out.stdout, out.stderr)
        })?;
        self.pass("QEMU Cortex-M3 boots the atto.core .atto image: 6 result vectors incl. entry(5) == 42");
        Ok(())
    }

    fn boot_trap(&mut self, image: &Path) -> GateResult<()> {
        let out = self.qemu(image)?;
        let console = out.console();
        // The fault must happen after the probe starts (not from a broken vector
        // table or reset path), exactly once, and never fall through.
        let started = audit::line_index(&console, "ORES_CORE_TRAP_PROBE start");
        let trapped = audit::line_index(&console, TRAP_LINE);
        require(
            out.status.is_some_and(|s| s != 0)
                && matches!((started, trapped), (Some(s), Some(t)) if s < t)
                && console.matches(TRAP_LINE).count() == 1
                && !console.contains("TRAP_PROBE_FAIL")
                && !console.contains("ORES_CORE_UNEXPECTED_EXCEPTION"),
            || format!("checked overflow did not reach the platform trap hook (status {:?}):\n{}{}",
                out.status, out.stdout, out.stderr),
        )?;
        self.pass("checked overflow traps into the platform failure hook, no OS abort");
        Ok(())
    }

    fn compiler_rejections(&mut self) -> GateResult<()> {
        let cases: &[(&str, &str, &str, &str)] = &[
            (
                "atto.std import under the core profile",
                "core",
                "import module testing from \"atto.std/testing\";\npub fnc p(int x): int { return x; }\n",
                "atto.std",
            ),
            (
                "legacy ores.std import under the core profile",
                "core",
                "import module testing from \"ores.std/testing\";\npub fnc p(int x): int { return x; }\n",
                "ores.std",
            ),
            (
                "unknown atto.core import",
                "core",
                "import module missing from \"atto.core/missing\";\npub fnc p(int x): int { return x; }\n",
                "unknown compiler-owned atto.core module",
            ),
            (
                "path traversal inside atto.core import",
                "core",
                "import module numeric from \"atto.core/../numeric\";\npub fnc p(int x): int { return x; }\n",
                "unknown compiler-owned atto.core module",
            ),
            (
                "transitive allocator requirement without a granted provider",
                "core",
                "import module b from \"atto.alloc/int_buffer_bundle\";\npub fnc p(int x): int { return b.marker(); }\n",
                "allocator",
            ),
            (
                "ambient project import under the core profile",
                "core",
                "import module local from \"./local\";\npub fnc p(int x): int { return local.answer(x); }\n",
                "ores.core",
            ),
            (
                "std profile on a freestanding target",
                "std",
                "pub fnc p(int x): int { return x; }\n",
                "freestanding targets use ores.core",
            ),
        ];
        write(
            &self.work.join("local.atto"),
            "define module local as\n  pub fnc answer(int x): int { return x; }\nend\n",
        )?;
        for (index, (label, profile, text, needle)) in cases.iter().enumerate() {
            let source = self.work.join(format!("reject_{index}.atto"));
            write(&source, text)?;
            let out = exec(
                Command::new(&self.tools.compiler)
                    .arg(format!("--target={TARGET}"))
                    .arg(format!("--library-profile={profile}"))
                    .arg(&source),
                None,
            )?;
            require(
                out.status.is_some_and(|s| s != 0) && out.stdout.is_empty() && out.stderr.contains(needle),
                || format!("compiler admitted {label} (status {:?}, stderr: {})", out.status, out.stderr.trim()),
            )?;
            self.pass(&format!("compiler rejects {label}"));
        }
        Ok(())
    }

    fn auditor_self_test(&mut self, startup: &Path) -> GateResult<()> {
        let object = self.work.join("forbidden_probe.o");
        let mut cmd = self.clang_arm();
        cmd.args(["-ffreestanding", "-w", "-c"])
            .arg(self.fixtures.join("forbidden_probe.c"))
            .arg("-o")
            .arg(&object);
        require_ok(exec(&mut cmd, None)?, "clang forbidden probe")?;
        let violations = audit::audit_guest_object(&self.nm(&object)?);
        for expected in ["malloc", "printf", "pthread_create", "ores_actor_spawn"] {
            require(violations.iter().any(|v| v.contains(&format!("`{expected}`"))), || {
                format!("symbol audit missed `{expected}` in a hosted object: {violations:?}")
            })?;
        }
        self.pass("symbol audit flags allocator, stdio, thread and actor-runtime references");

        let (_, out) = self.link("forbidden.elf", &[startup, &object])?;
        require(out.status.is_some_and(|s| s != 0) && out.stderr.contains("undefined symbol"), || {
            "a hosted object linked into the no-libc image; the link is not freestanding".into()
        })?;
        self.pass("no-libc link refuses a hosted object");
        Ok(())
    }

    fn clang_arm(&self) -> Command {
        let mut cmd = Command::new(&self.tools.clang);
        cmd.arg(format!("--target={TARGET}"))
            .args(["-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft", "-O2"]);
        cmd
    }

    fn nm(&self, file: &Path) -> GateResult<Vec<audit::NmSymbol>> {
        let out = exec(Command::new(&self.tools.llvm_nm).arg(file), None)?;
        require(out.status == Some(0), || format!("llvm-nm {}: {}", file.display(), out.stderr))?;
        Ok(audit::parse_nm(&out.stdout))
    }
}

fn require(condition: bool, message: impl FnOnce() -> String) -> GateResult<()> {
    if condition { Ok(()) } else { Err(message()) }
}

fn require_ok(out: Output, what: &str) -> GateResult<()> {
    require(out.status == Some(0), || format!("{what} failed (status {:?}):\n{}", out.status, out.stderr))
}

fn write(path: &Path, text: &str) -> GateResult<()> {
    std::fs::write(path, text).map_err(|e| format!("write {}: {e}", path.display()))
}

/// Run a command, capturing output. With a timeout, the child is killed and
/// the run fails if it does not exit in time (a hung guest is a failure).
fn exec(cmd: &mut Command, timeout: Option<Duration>) -> GateResult<Output> {
    let display = format!("{cmd:?}");
    let mut child = cmd
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("could not start {display}: {e}"))?;
    let mut stdout_pipe = child.stdout.take().expect("piped stdout");
    let mut stderr_pipe = child.stderr.take().expect("piped stderr");
    let stdout_reader = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = stdout_pipe.read_to_string(&mut s);
        s
    });
    let stderr_reader = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = stderr_pipe.read_to_string(&mut s);
        s
    });
    let deadline = timeout.map(|t| Instant::now() + t);
    let status = loop {
        if let Some(status) = child.try_wait().map_err(|e| format!("wait {display}: {e}"))? {
            break status;
        }
        if deadline.is_some_and(|d| Instant::now() >= d) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!("{display} timed out after {timeout:?}"));
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    Ok(Output {
        status: status.code(),
        stdout: stdout_reader.join().unwrap_or_default(),
        stderr: stderr_reader.join().unwrap_or_default(),
    })
}
