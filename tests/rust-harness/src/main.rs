mod checked_i64;

use std::{
    env,
    ffi::OsStr,
    fs,
    io,
    path::{Path, PathBuf},
    process::{Command, Output},
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(prefix: &str) -> io::Result<Self> {
        let counter = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        let name = format!("{prefix}{}-{nanos}-{counter}", std::process::id());
        let path = env::temp_dir().join(name);
        fs::create_dir(&path)?;
        Ok(Self { path })
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn run<I, S>(program: &Path, args: I) -> Result<Output, String>
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    Command::new(program)
        .args(args)
        .output()
        .map_err(|e| format!("failed to run {}: {e}", program.display()))
}

fn stdout(out: &Output) -> String {
    String::from_utf8_lossy(&out.stdout).into_owned()
}

fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

fn require(condition: bool, message: impl Into<String>) -> Result<(), String> {
    if condition {
        Ok(())
    } else {
        Err(message.into())
    }
}

fn json_value<'a>(text: &'a str, key: &str) -> Option<&'a str> {
    let needle = format!("\"{key}\"");
    let start = text.find(&needle)? + needle.len();
    let rest = text.get(start..)?;
    let colon = rest.find(':')?;
    Some(rest.get(colon + 1..)?.trim_start())
}

fn json_bool(text: &str, key: &str) -> Option<bool> {
    let value = json_value(text, key)?;
    if value.starts_with("true") {
        Some(true)
    } else if value.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

fn json_u64(text: &str, key: &str) -> Option<u64> {
    let value = json_value(text, key)?;
    let digits: String = value.chars().take_while(|c| c.is_ascii_digit()).collect();
    digits.parse().ok()
}

fn json_string(text: &str, key: &str) -> Option<String> {
    let value = json_value(text, key)?;
    let quoted = value.strip_prefix('"')?;
    let end = quoted.find('"')?;
    Some(quoted[..end].to_string())
}

#[cfg(unix)]
fn symlink_file(src: &Path, dst: &Path) -> io::Result<()> {
    std::os::unix::fs::symlink(src, dst)
}

#[cfg(windows)]
fn symlink_file(src: &Path, dst: &Path) -> io::Result<()> {
    std::os::windows::fs::symlink_file(src, dst)
}

fn realtime_profile(compiler: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-rt-profile-").map_err(|e| e.to_string())?;
    let root = &temp.path;

    let pure = root.join("pure.ores");
    fs::write(
        &pure,
        "fnc helper(): int { return 41; }\nfnc main(): int { return helper(); }\n",
    )
    .map_err(|e| e.to_string())?;

    let out = run(compiler, [OsStr::new("--rt-check"), pure.as_os_str()])?;
    require(out.status.success(), stderr(&out))?;
    let report = stdout(&out);
    require(json_bool(&report, "source_admitted") == Some(true), "pure source must admit")?;
    require(
        json_bool(&report, "artifact_certified") == Some(false),
        "compiler check must not certify linked artifact",
    )?;
    require(
        json_u64(&report, "reachable_functions") == Some(2),
        "transitive reachable count",
    )?;

    let report_path = root.join("report.json");
    let report_flag = format!("--rt-report={}", report_path.display());
    let out = run(
        compiler,
        [
            OsStr::new("--profile=native-hard-rt"),
            OsStr::new(&report_flag),
            pure.as_os_str(),
        ],
    )?;
    require(out.status.success(), stderr(&out))?;
    require(stdout(&out).contains("define"), "admitted profile must emit LLVM IR")?;
    let persisted = fs::read_to_string(&report_path).map_err(|e| e.to_string())?;
    require(
        json_string(&persisted, "diagnostic").as_deref() == Some("OK"),
        "persisted report",
    )?;

    let io_source = root.join("io.ores");
    fs::write(
        &io_source,
        "fnc io_capable(): int using io { return 1; }\nfnc main(): int { return io_capable(); }\n",
    )
    .map_err(|e| e.to_string())?;
    let out = run(compiler, [OsStr::new("--rt-check"), io_source.as_os_str()])?;
    require(out.status.code() == Some(1), "declared IO must reject")?;
    let report = stdout(&out);
    require(
        json_bool(&report, "source_admitted") == Some(false),
        "IO report admission",
    )?;
    require(
        json_string(&report, "diagnostic").as_deref() != Some("OK"),
        "IO diagnostic",
    )?;

    let recursive = root.join("recursive.ores");
    fs::write(
        &recursive,
        "fnc bounce(): int { return bounce(); }\nfnc main(): int { return bounce(); }\n",
    )
    .map_err(|e| e.to_string())?;
    let out = run(compiler, [OsStr::new("--rt-check"), recursive.as_os_str()])?;
    require(out.status.code() == Some(1), "recursive graph must reject")?;
    require(
        json_string(&stdout(&out), "diagnostic").as_deref() == Some("RT008_UNBOUNDED_LOOP"),
        "recursion diagnostic",
    )?;

    let out = run(compiler, [OsStr::new("--rt-entry=helper/0"), pure.as_os_str()])?;
    require(
        out.status.code() == Some(2),
        "rt-entry without an RT analysis mode must reject",
    )?;

    let original = fs::read_to_string(&pure).map_err(|e| e.to_string())?;
    let alias = root.join("pure-report-alias.ores");
    symlink_file(&pure, &alias).map_err(|e| format!("create symlink: {e}"))?;
    let alias_flag = format!("--rt-report={}", alias.display());
    let out = run(
        compiler,
        [OsStr::new("--rt-check"), OsStr::new(&alias_flag), pure.as_os_str()],
    )?;
    require(
        out.status.code() == Some(1),
        "report symlink aliasing source must reject",
    )?;
    require(
        fs::read_to_string(&pure).map_err(|e| e.to_string())? == original,
        "source must not be overwritten through report symlink",
    )?;

    let hard_alias = root.join("pure-report-hardlink.ores");
    fs::hard_link(&pure, &hard_alias).map_err(|e| format!("create hard link: {e}"))?;
    let hard_flag = format!("--rt-report={}", hard_alias.display());
    let out = run(
        compiler,
        [OsStr::new("--rt-check"), OsStr::new(&hard_flag), pure.as_os_str()],
    )?;
    require(
        out.status.code() == Some(1),
        "report hardlink aliasing source must reject",
    )?;
    require(
        fs::read_to_string(&pure).map_err(|e| e.to_string())? == original,
        "source must not be overwritten through report hardlink",
    )?;

    println!("native hard-RT profile CLI: PASS");
    Ok(())
}

fn callable_nominal(compiler: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-callable-nominal-").map_err(|e| e.to_string())?;
    let source = temp.path.join("boundary.ores");
    let mut negatives: Vec<(String, Option<&str>)> = Vec::new();

    for name in ["Callable", "Fnc", "Function", "Routine", "Method"] {
        negatives.push((format!("define class {name} as end\n"), Some("nominal class types")));
    }
    negatives.push((
        "define actor Callable as\n  let count: int = 0;\n  pub fnc on_message(int value): int { return value; }\nend\n".to_string(),
        Some("nominal actor types"),
    ));
    for clause in [
        "extends Callable",
        "extends Fnc<int, int>",
        "implements Callable",
        "implements Fnc<int, int>",
    ] {
        negatives.push((format!("define class Child {clause} as end\n"), None));
    }

    let entry = "pub fnc main(): int { return 0; }\n";
    for (body, diagnostic) in &negatives {
        fs::write(&source, format!("{body}{entry}")).map_err(|e| e.to_string())?;
        let out = run(compiler, [source.as_os_str()])?;
        require(
            !out.status.success() && stdout(&out).trim().is_empty(),
            format!(
                "unsupported callable nominal form admitted: {body:?}; status={:?}; stdout={}",
                out.status.code(),
                stdout(&out)
            ),
        )?;
        if let Some(diagnostic) = diagnostic {
            require(
                stderr(&out).contains(diagnostic),
                format!("missing native diagnostic for {body:?}: {}", stderr(&out)),
            )?;
        }
    }

    fs::write(
        &source,
        "define class Ordinary as\n  pub let int value = 1;\nend\npub fnc main(): int { return 0; }\n",
    )
    .map_err(|e| e.to_string())?;
    let out = run(compiler, [source.as_os_str()])?;
    require(
        out.status.success()
            && stderr(&out).is_empty()
            && stdout(&out).starts_with("; ModuleID"),
        format!(
            "ordinary class affected: stderr={} stdout={}",
            stderr(&out),
            stdout(&out)
        ),
    )?;

    println!(
        "PASS {} negative callable-nominal tests and ordinary class",
        negatives.len()
    );
    Ok(())
}

fn nominal_type_binders(compiler: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-nominal-type-binders-").map_err(|e| e.to_string())?;
    let source = temp.path.join("nominal-binder.ores");
    let cases = [
        "define class _ as end\n",
        "define actor _ as\n  let count: int = 0;\n  pub fnc on_message(int value): int { return value; }\nend\n",
        "define contract _ as\n  fnc execute(int value) => int;\nend\n",
    ];
    for body in cases {
        let full_source = format!("{body}pub fnc main(): int {{ return 0; }}\n");
        fs::write(&source, full_source).map_err(|e| e.to_string())?;
        let out = run(compiler, [source.as_os_str()])?;
        require(
            !out.status.success()
                && stdout(&out).trim().is_empty()
                && stderr(&out).contains("wildcard '_' cannot be declared as a nominal type name"),
            format!(
                "wildcard nominal declaration must fail before IR emission: {body:?}; status={:?}; stdout={}; stderr={}",
                out.status.code(), stdout(&out), stderr(&out)
            ),
        )?;
    }

    fs::write(
        &source,
        "define class Ordinary as\n  pub let int value = 1;\nend\npub fnc main(): int { return 0; }\n",
    ).map_err(|e| e.to_string())?;
    let good = run(compiler, [source.as_os_str()])?;
    require(
        good.status.success() && stderr(&good).is_empty() && stdout(&good).starts_with("; ModuleID"),
        format!("ordinary nominal declaration regressed: stdout={} stderr={}", stdout(&good), stderr(&good)),
    )?;

    println!("PASS 3 negative native nominal wildcard declarations and one positive class");
    Ok(())
}


// These parsers validate the linked native container, not the semantics of
// guest execution. Every OS format is exercised on every CI runner so that
// Linux-only CI cannot accidentally green-light incomplete PE/Mach-O gates.
fn assert_native_aot_artifact_for(
    image: &[u8],
    os: &str,
    arch: &str,
) -> Result<(), String> {
    require(image.len() >= 64, "native AOT artifact is too small")?;
    match os {
        "linux" => {
            require(image.starts_with(b"\x7fELF"), "expected ELF native executable")?;
            require(image[4] == 2 && image[5] == 1 && image[6] == 1,
                "expected little-endian ELF64 with current ident version")?;
            let kind = u16::from_le_bytes([image[16], image[17]]);
            require(kind == 2 || kind == 3, "expected ELF executable or PIE, not an object")?;
            let expected_machine = match arch {
                "x86_64" => 62,
                "aarch64" => 183,
                other => return Err(format!("unsupported native AOT architecture: {other}")),
            };
            let machine = u16::from_le_bytes([image[18], image[19]]);
            require(machine == expected_machine, format!("wrong ELF machine: {machine}"))?;
            let elf_version = u32::from_le_bytes(image[20..24].try_into().unwrap());
            require(elf_version == 1, "unsupported ELF version")?;
            let entry = u64::from_le_bytes(image[24..32].try_into().unwrap());
            require(entry != 0, "ELF has no native entrypoint")?;
            let phoff = u64::from_le_bytes(image[32..40].try_into().unwrap()) as usize;
            let ehsize = u16::from_le_bytes(image[52..54].try_into().unwrap());
            let phentsize = u16::from_le_bytes(image[54..56].try_into().unwrap()) as usize;
            let phnum = u16::from_le_bytes(image[56..58].try_into().unwrap()) as usize;
            require(ehsize == 64 && phentsize == 56 && phnum != 0 && phoff >= 64,
                "ELF header or program-header table is invalid")?;
            let phend = phoff.checked_add(phnum.checked_mul(phentsize)
                .ok_or("ELF program-header table overflow")?)
                .ok_or("ELF program-header offset overflow")?;
            require(phend <= image.len(), "ELF program headers exceed file bounds")?;
            let mut executable_load = false;
            for index in 0..phnum {
                let pos = phoff + index * phentsize;
                let p_type = u32::from_le_bytes(image[pos..pos + 4].try_into().unwrap());
                let p_flags = u32::from_le_bytes(image[pos + 4..pos + 8].try_into().unwrap());
                if p_type != 1 {
                    continue;
                }
                let offset = u64::from_le_bytes(image[pos + 8..pos + 16].try_into().unwrap());
                let vaddr = u64::from_le_bytes(image[pos + 16..pos + 24].try_into().unwrap());
                let size = u64::from_le_bytes(image[pos + 32..pos + 40].try_into().unwrap());
                let memsz = u64::from_le_bytes(image[pos + 40..pos + 48].try_into().unwrap());
                require(size <= memsz, "ELF PT_LOAD filesz exceeds memsz")?;
                require(offset.checked_add(size)
                    .is_some_and(|end| end <= image.len() as u64),
                    "ELF PT_LOAD extends outside the artifact")?;
                // An arbitrary nonzero e_entry and an unrelated executable
                // PT_LOAD do not constitute an executable. The native entry
                // must be in a file-backed executable load region.
                if p_flags & 1 != 0 && size > 0 &&
                    entry.checked_sub(vaddr).is_some_and(|delta| delta < size) {
                    executable_load = true;
                }
            }
            require(executable_load,
                "ELF entrypoint is not mapped by a file-backed executable PT_LOAD")?;
        }
        "macos" => {
            require(&image[..4] == b"\xcf\xfa\xed\xfe", "expected thin Mach-O 64-bit executable")?;
            let expected_cpu = match arch {
                "x86_64" => 0x0100_0007u32,
                "aarch64" => 0x0100_000cu32,
                other => return Err(format!("unsupported native AOT architecture: {other}")),
            };
            let cpu = u32::from_le_bytes(image[4..8].try_into().unwrap());
            require(cpu == expected_cpu, format!("wrong Mach-O architecture: {cpu}"))?;
            let file_type = u32::from_le_bytes(image[12..16].try_into().unwrap());
            require(file_type == 2, "expected MH_EXECUTE, not a Mach-O object or dylib")?;
            let commands = u32::from_le_bytes(image[16..20].try_into().unwrap()) as usize;
            let command_bytes = u32::from_le_bytes(image[20..24].try_into().unwrap()) as usize;
            let end = 32usize.checked_add(command_bytes).ok_or("Mach-O commands overflow")?;
            require(commands > 0 && commands <= command_bytes / 8 && end <= image.len(),
                "Mach-O load command table missing or out of bounds")?;
            let mut pos = 32usize;
            let mut has_segment = false;
            let mut has_entry = false;
            for _ in 0..commands {
                require(pos.checked_add(8).is_some_and(|next| next <= end),
                    "truncated Mach-O load command")?;
                let cmd = u32::from_le_bytes(image[pos..pos + 4].try_into().unwrap());
                let cmdsize = u32::from_le_bytes(image[pos + 4..pos + 8].try_into().unwrap()) as usize;
                require(cmdsize >= 8 && pos.checked_add(cmdsize)
                    .is_some_and(|next| next <= end), "invalid Mach-O command size")?;
                if cmd == 0x19 {
                    require(cmdsize >= 72, "truncated 64-bit Mach-O segment command")?;
                    let initprot = u32::from_le_bytes(image[pos + 60..pos + 64].try_into().unwrap());
                    if initprot & 4 != 0 { has_segment = true; }
                }
                if cmd == 0x80000028 || cmd == 0x5 { has_entry = true; }
                pos += cmdsize;
            }
            require(pos == end && has_segment && has_entry,
                "Mach-O missing executable segment or native entry command")?;
        }
        "windows" => {
            require(image.starts_with(b"MZ"), "expected PE executable")?;
            let pe_offset = u32::from_le_bytes(image[60..64].try_into().unwrap()) as usize;
            require(pe_offset >= 64 && pe_offset <= image.len().saturating_sub(24),
                "PE signature or COFF header falls outside the artifact")?;
            require(&image[pe_offset..pe_offset + 4] == b"PE\0\0", "expected a valid PE signature")?;
            let machine = u16::from_le_bytes(image[pe_offset + 4..pe_offset + 6].try_into().unwrap());
            let expected = match arch {
                "x86_64" => 0x8664u16,
                "aarch64" => 0xaa64u16,
                other => return Err(format!("unsupported native AOT architecture: {other}")),
            };
            require(machine == expected, format!("wrong PE architecture: {machine}"))?;
            let sections = u16::from_le_bytes(image[pe_offset + 6..pe_offset + 8].try_into().unwrap()) as usize;
            let optional_size = u16::from_le_bytes(image[pe_offset + 20..pe_offset + 22].try_into().unwrap()) as usize;
            let flags = u16::from_le_bytes(image[pe_offset + 22..pe_offset + 24].try_into().unwrap());
            require(flags & 0x0002 != 0 && flags & 0x2000 == 0,
                "PE artifact is not an executable or is a DLL")?;
            require(sections != 0 && optional_size >= 112,
                "PE is missing its sections or PE32+ optional header")?;
            let opt_start = pe_offset + 24;
            let opt_end = opt_start.checked_add(optional_size).ok_or("PE optional header overflow")?;
            let section_end = opt_end.checked_add(sections.checked_mul(40)
                .ok_or("PE section-table size overflow")?)
                .ok_or("PE section-table offset overflow")?;
            require(section_end <= image.len(), "PE optional header or section table out of bounds")?;
            let magic = u16::from_le_bytes(image[opt_start..opt_start + 2].try_into().unwrap());
            require(magic == 0x20b, "expected PE32+ optional header")?;
            let entry = u32::from_le_bytes(image[opt_start + 16..opt_start + 20].try_into().unwrap());
            require(entry != 0, "PE executable has no native entrypoint")?;
        }
        other => return Err(format!("unqualified native AOT host OS: {other}")),
    }
    Ok(())
}

fn assert_native_aot_artifact(image: &[u8]) -> Result<(), String> {
    assert_native_aot_artifact_for(image, std::env::consts::OS, std::env::consts::ARCH)
}

// Host-independent negative checks are distinct from the real compiler -> Clang
// -> executable -> guest-result test. These byte probes must never be reported
// as a guest AOT success.
fn aot_artifact_header_selftest() -> Result<(), String> {
    for (name, payload) in [
        ("empty", vec![]),
        ("shell", b"#!/bin/sh\nexit 42\n".to_vec()),
        ("zeros", vec![0u8; 128]),
    ] {
        for (os, arch) in [("linux", "x86_64"), ("macos", "aarch64"), ("windows", "x86_64")] {
            require(assert_native_aot_artifact_for(&payload, os, arch).is_err(),
                format!("admitted forged native artifact: {name} on {os}"))?;
        }
    }

    let mut elf = vec![0u8; 256];
    elf[..4].copy_from_slice(b"\x7fELF");
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    elf[16..18].copy_from_slice(&3u16.to_le_bytes());
    elf[18..20].copy_from_slice(&62u16.to_le_bytes());
    elf[20..24].copy_from_slice(&1u32.to_le_bytes());
    elf[32..40].copy_from_slice(&64u64.to_le_bytes());
    elf[52..54].copy_from_slice(&64u16.to_le_bytes());
    elf[54..56].copy_from_slice(&56u16.to_le_bytes());
    elf[56..58].copy_from_slice(&1u16.to_le_bytes());
    elf[64..68].copy_from_slice(&1u32.to_le_bytes());
    elf[68..72].copy_from_slice(&5u32.to_le_bytes());
    elf[72..80].copy_from_slice(&128u64.to_le_bytes());
    elf[96..104].copy_from_slice(&32u64.to_le_bytes());
    require(assert_native_aot_artifact_for(&elf, "linux", "x86_64").is_err(),
        "admitted ELF ET_DYN object without a native entrypoint")?;
    // Structurally valid synthetic header: not executable guest evidence.
    elf[24..32].copy_from_slice(&4096u64.to_le_bytes());
    elf[80..88].copy_from_slice(&4096u64.to_le_bytes());
    elf[104..112].copy_from_slice(&32u64.to_le_bytes());
    require(assert_native_aot_artifact_for(&elf, "linux", "x86_64").is_ok(),
        "denied a structurally valid ELF header fixture")?;
    elf[24..32].copy_from_slice(&8192u64.to_le_bytes());
    require(assert_native_aot_artifact_for(&elf, "linux", "x86_64").is_err(),
        "admitted executable segment with an unrelated ELF entrypoint")?;
    elf[24..32].copy_from_slice(&4096u64.to_le_bytes());
    elf[104..112].copy_from_slice(&31u64.to_le_bytes());
    require(assert_native_aot_artifact_for(&elf, "linux", "x86_64").is_err(),
        "admitted ELF PT_LOAD filesz greater than memsz")?;
    elf[104..112].copy_from_slice(&32u64.to_le_bytes());
    require(assert_native_aot_artifact_for(&elf, "linux", "aarch64").is_err(),
        "admitted ELF binary with wrong architecture")?;
    elf[68..72].copy_from_slice(&4u32.to_le_bytes());
    require(assert_native_aot_artifact_for(&elf, "linux", "x86_64").is_err(),
        "admitted ELF without an executable PT_LOAD segment")?;

    let mut pe = vec![0u8; 256];
    pe[..2].copy_from_slice(b"MZ");
    pe[60..64].copy_from_slice(&128u32.to_le_bytes());
    pe[128..132].copy_from_slice(b"PE\0\0");
    pe[132..134].copy_from_slice(&0x8664u16.to_le_bytes());
    pe[134..136].copy_from_slice(&1u16.to_le_bytes());
    pe[150..152].copy_from_slice(&2u16.to_le_bytes());
    require(assert_native_aot_artifact_for(&pe, "windows", "x86_64").is_err(),
        "admitted PE with executable flag but without a PE32+ optional header")?;
    pe[150..152].copy_from_slice(&0x2002u16.to_le_bytes());
    require(assert_native_aot_artifact_for(&pe, "windows", "x86_64").is_err(),
        "admitted a PE DLL as an executable")?;
    pe[60..64].copy_from_slice(&253u32.to_le_bytes());
    require(assert_native_aot_artifact_for(&pe, "windows", "x86_64").is_err(),
        "admitted truncated PE signature or COFF header")?;

    let mut macho = vec![0u8; 128];
    macho[..4].copy_from_slice(b"\xcf\xfa\xed\xfe");
    macho[4..8].copy_from_slice(&0x0100_000cu32.to_le_bytes());
    macho[12..16].copy_from_slice(&2u32.to_le_bytes());
    require(assert_native_aot_artifact_for(&macho, "macos", "aarch64").is_err(),
        "admitted Mach-O MH_EXECUTE without load commands")?;
    macho[16..20].copy_from_slice(&1u32.to_le_bytes());
    macho[20..24].copy_from_slice(&8u32.to_le_bytes());
    macho[32..36].copy_from_slice(&0x19u32.to_le_bytes());
    macho[36..40].copy_from_slice(&8u32.to_le_bytes());
    require(assert_native_aot_artifact_for(&macho, "macos", "aarch64").is_err(),
        "admitted truncated Mach-O segment command")?;

    let host = std::env::current_exe().map_err(|e| format!("resolve native test harness: {e}"))?;
    let bytes = fs::read(&host).map_err(|e| format!("read native test harness: {e}"))?;
    assert_native_aot_artifact(&bytes)?;
    println!("native AOT artifact format, architecture and adversarial tests: PASS (host binary only)");
    Ok(())
}

fn aot_driver(driver: &Path, compiler: &Path, clang: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-aot-").map_err(|e| e.to_string())?;
    let source = temp.path.join("main.ores");
    let output = temp.path.join("native app");

    fs::write(&source, "fnc main(): int { return 42; }\n").map_err(|e| e.to_string())?;
    let status = Command::new(driver)
        .arg(&source)
        .arg(&output)
        .env("ORESLANG_LLVMC", compiler)
        .env("CLANG", clang)
        .status()
        .map_err(|e| format!("run AOT driver: {e}"))?;
    require(status.success(), format!("AOT driver failed: {status}"))?;

    let original = fs::read(&output).map_err(|e| e.to_string())?;
    assert_native_aot_artifact(&original)?;
    let status = Command::new(&output)
        .env_clear()
        .env("PATH", "/nonexistent")
        .status()
        .map_err(|e| format!("run native output: {e}"))?;
    require(
        status.code() == Some(42),
        format!("native output returned {:?}", status.code()),
    )?;

    let original = fs::read(&output).map_err(|e| e.to_string())?;
    // A host-side trampoline that always returns 42 can pass one smoke test.
    // Distinct guest programs must produce distinct, independently runnable
    // linked artifacts, with the compiler and PATH unavailable at execution.
    // These tests establish only the currently implemented scalar guest slice,
    // not full language/actor AOT parity.
    for (label, program, expected_exit) in [
        ("zero", "pub routine main(): int { return 0; }\n", 0),
        ("one", "pub routine main(): int { return 1; }\n", 1),
        ("module", "define module demo as\n  pub fnc main(): int { return 7; }\nend\n", 7),
        ("forward-call", "pub fnc main(): int { return add(40, 2); }\nfnc add(int a, int b): int { return a + b; }\n", 42),
    ] {
        fs::write(&source, program).map_err(|e| format!("{label}: write guest: {e}"))?;
        let built = Command::new(driver)
            .arg(&source).arg(&output)
            .env("ORESLANG_LLVMC", compiler).env("CLANG", clang)
            .output().map_err(|e| format!("{label}: AOT driver: {e}"))?;
        require(built.status.success(),
            format!("{label}: native link failed: {}", stderr(&built)))?;
        let executed = Command::new(&output)
            .env_clear().env("PATH", "/nonexistent")
            .output().map_err(|e| format!("{label}: native entrypoint: {e}"))?;
        require(executed.status.code() == Some(expected_exit),
            format!("{label}: native guest returned {:?} instead of {expected_exit}: {}",
                executed.status.code(), stderr(&executed)))?;
        require(executed.stdout.is_empty() && executed.stderr.is_empty(),
            format!("{label}: unexpected guest I/O or host interpreter diagnostics"))?;
        if expected_exit != 42 {
            require(fs::read(&output).map_err(|e| e.to_string())? != original,
                format!("{label}: artifact identical to baseline despite distinct guest return"))?;
        }
    }
    // Source actors through the same driver: the IR declares the native actor
    // runtime, so the driver must link the scheduler/VM/actor heap itself.
    // Two distinct returns prove the exit code comes from the guest.
    let actor_source = temp.path.join("actor.atto");
    for (label, done_await, expected_exit) in [("actor-ready", false, 44), ("actor-done", true, 45)] {
        let program = format!(
            "define actor Worker as {{\n  let total: int = 0;\n  pub fnc on_message(int n): int {{\n    total = total + n;\n    return total;\n  }}\n}}\n\
             pub fnc main(): int {{\n  const worker: ActorRef<Worker> = spawn Worker();\n{}  stop worker;\n{}  return {expected_exit};\n}}\n",
            if done_await { "" } else {
                "  const Future<void> ready = worker.get_ready();\n  await ready;\n  worker.send(1);\n  worker.send(2);\n"
            },
            if done_await { "  await worker.get_done();\n" } else { "" });
        fs::write(&actor_source, program).map_err(|e| format!("{label}: write guest: {e}"))?;
        let built = Command::new(driver)
            .arg(&actor_source).arg(&output)
            .env("ORESLANG_LLVMC", compiler).env("CLANG", clang)
            .output().map_err(|e| format!("{label}: AOT driver: {e}"))?;
        require(built.status.success(),
            format!("{label}: actor runtime link failed: {}", stderr(&built)))?;
        let executed = Command::new(&output)
            .env_clear().env("PATH", "/nonexistent")
            .output().map_err(|e| format!("{label}: native entrypoint: {e}"))?;
        require(executed.status.code() == Some(expected_exit),
            format!("{label}: native actor guest returned {:?} instead of {expected_exit}: {}",
                executed.status.code(), stderr(&executed)))?;
    }

    // Canonical spelling shared with the Java frontend: `as ... end`, a
    // type-first field, `self.field`, a bare method and `worker.stop()`.
    // The same bytes must run on both backends.
    fs::write(&actor_source, "define actor Worker as\n  let int total = 0;\n  pub on_message(int n): int {\n    self.total = self.total + n;\n    return self.total;\n  }\nend\n\
pub fnc main(): int {\n  const worker: ActorRef<Worker> = spawn Worker();\n  worker.stop();\n  await worker.get_done();\n  return 46;\n}\n")
        .map_err(|e| format!("canonical actor: write guest: {e}"))?;
    let built = Command::new(driver)
        .arg(&actor_source).arg(&output)
        .env("ORESLANG_LLVMC", compiler).env("CLANG", clang)
        .output().map_err(|e| format!("canonical actor: AOT driver: {e}"))?;
    require(built.status.success(),
        format!("canonical actor: compile/link failed: {}", stderr(&built)))?;
    let executed = Command::new(&output)
        .env_clear().env("PATH", "/nonexistent")
        .output().map_err(|e| format!("canonical actor: native entrypoint: {e}"))?;
    require(executed.status.code() == Some(46),
        format!("canonical actor guest returned {:?} instead of 46", executed.status.code()))?;

    // Rebuild the original image before negative cases: they must preserve
    // exactly these published bytes and never publish a partial executable.
    fs::write(&source, "fnc main(): int { return 42; }\n")
        .map_err(|e| e.to_string())?;
    let restored = Command::new(driver)
        .arg(&source).arg(&output)
        .env("ORESLANG_LLVMC", compiler).env("CLANG", clang)
        .output().map_err(|e| format!("restore linked guest: {e}"))?;
    require(restored.status.success(),
        format!("restore linked guest failed: {}", stderr(&restored)))?;
    let original = fs::read(&output).map_err(|e| e.to_string())?;
    for text in [
        "import module spin from \"spin\";\n",
        "this is not Oreslang\n",
        "fnc helper(): int { return 42; }\n",
    ] {
        fs::write(&source, text).map_err(|e| e.to_string())?;
        let out = Command::new(driver)
            .arg(&source)
            .arg(&output)
            .env("ORESLANG_LLVMC", compiler)
            .env("CLANG", clang)
            .output()
            .map_err(|e| format!("run rejected AOT case: {e}"))?;
        require(!out.status.success(), format!("invalid source admitted: {text:?}"))?;
        require(
            fs::read(&output).map_err(|e| e.to_string())? == original,
            "failed compile changed existing native output",
        )?;
        let absent = temp.path.join("rejected native app");
        let rejected = Command::new(driver)
            .arg(&source)
            .arg(&absent)
            .env("ORESLANG_LLVMC", compiler)
            .env("CLANG", clang)
            .output()
            .map_err(|e| format!("run rejected fresh output: {e}"))?;
        require(!rejected.status.success(), "invalid source admitted for a fresh output")?;
        require(!absent.exists(), "invalid source published a new native artifact")?;
    }

    // The IR screen is a release gate, not linked-image certification. A compiler
    // emitting collector IR must be stopped before Clang, by this specific gate.
    fs::write(&source, "fnc main(): int { return 42; }\n").map_err(|e| e.to_string())?;
    let fake = temp.path.join("fake-ir-compiler.sh");
    for ir in [
        r#"define void @guest() gc "shadow-stack" { ret void }"#,
        r#"define void @"safe;label"() gc "shadow-stack" { ret void }"#,
        "declare void @llvm.gcroot(ptr, ptr)",
        "declare token @llvm.gc.statepoint.p0(i64, i32, ptr, i32, i32, ...)",
        "declare ptr @GC_malloc(i64)",
        "declare void @gc_collect()",
        r#"declare void @"llvm.g\63root"(ptr, ptr)"#,
    ] {
        fs::write(&fake, format!("#!/bin/sh\ncat <<'IR'\n{ir}\nIR\n")).map_err(|e| e.to_string())?;
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&fake, fs::Permissions::from_mode(0o755)).map_err(|e| e.to_string())?;
        }
        let out = Command::new(driver)
            .arg(&source)
            .arg(&output)
            .env("ORESLANG_LLVMC", &fake)
            .env("CLANG", clang)
            .output()
            .map_err(|e| format!("run collector IR case: {e}"))?;
        require(!out.status.success(), format!("collector IR admitted: {ir}"))?;
        require(
            stderr(&out).contains("STRICT_NO_GC_IR_REJECTED"),
            format!("{ir}: rejected by something other than the gate: {}", stderr(&out)),
        )?;
        require(
            fs::read(&output).map_err(|e| e.to_string())? == original,
            "collector IR changed existing native output",
        )?;
    }
    fs::remove_file(&fake).map_err(|e| e.to_string())?;

    for entry in fs::read_dir(&temp.path).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if entry
            .file_name()
            .to_string_lossy()
            .starts_with(".ores-aot.")
        {
            return Err(format!(
                "AOT staging file leaked: {}",
                entry.path().display()
            ));
        }
    }

    println!("native AOT driver: PASS");
    Ok(())
}


fn named_call_source_order(compiler: &Path, fixture: &Path) -> Result<(), String> {
    let out = run(compiler, [fixture.as_os_str()])?;
    require(out.status.success(), format!("compile named-call fixture: {}", stderr(&out)))?;
    let ir = stdout(&out);
    require(ir.starts_with("; ModuleID"), "expected native LLVM module header")?;
    let second = ir.find("call i64 @read_second(")
        .ok_or_else(|| "missing call to read_second".to_string())?;
    let first = ir.find("call i64 @read_first(")
        .ok_or_else(|| "missing call to read_first".to_string())?;
    require(second < first, "named arguments evaluated in formal rather than source order")?;
    require(stderr(&out).is_empty(), format!("unexpected compiler stderr: {}", stderr(&out)))?;
    println!("named-call source-order IR: PASS");
    Ok(())
}

fn actor_persistent_fields(compiler: &Path, clang: &Path, root: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-actor-fields-").map_err(|e| e.to_string())?;
    let fixture = root.join("tests/fixtures/actor-owned-fields.ores");
    let compiled = run(compiler, [fixture.as_os_str()])?;
    require(compiled.status.success(), format!("compile actor fields: {}", stderr(&compiled)))?;
    require(!compiled.stdout.is_empty(), "compiler returned no actor LLVM IR")?;
    let ir = temp.path.join("actor.ll");
    let binary = temp.path.join("actor");
    fs::write(&ir, &compiled.stdout).map_err(|e| e.to_string())?;
    let linked = Command::new(clang)
        .arg(&ir)
        .arg(root.join("tests/native_actor_source_harness.c"))
        .arg(root.join("src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"))
        .arg(format!("-I{}", root.join("src/main/cpp").display()))
        .arg("-o")
        .arg(&binary)
        .output()
        .map_err(|e| format!("run clang: {e}"))?;
    require(linked.status.success(), format!("link actor fields: {}", stderr(&linked)))?;
    let executed = run(&binary, std::iter::empty::<&OsStr>())?;
    require(executed.status.success(),
        format!("actor owned-field executable exited {:?}: {}", executed.status.code(), stderr(&executed)))?;
    println!("actor persistent fields: PASS");
    Ok(())
}

fn tagged_message_wire(binary: &Path, vectors: &Path) -> Result<(), String> {
    let contents = fs::read_to_string(vectors)
        .map_err(|e| format!("read {}: {e}", vectors.display()))?;
    require(json_string(&contents, "schema").as_deref() == Some("oreslang.tagged.message.v2"),
        "tagged-message vector schema mismatch")?;
    let expected = json_string(&contents, "hex")
        .ok_or_else(|| "missing hex field in tagged-message vector".to_string())?;
    require(!expected.is_empty() && expected.bytes().all(|b| b.is_ascii_hexdigit()) && expected.len() % 2 == 0,
        "tagged-message vector hex must be nonempty even-length hexadecimal")?;
    let actual = run(binary, std::iter::empty::<&OsStr>())?;
    require(actual.status.success(), format!("tagged-message executable failed: {}", stderr(&actual)))?;
    require(stdout(&actual).trim() == expected,
        format!("native enum message bytes differ from Java fixture: {} != {expected}", stdout(&actual).trim()))?;
    println!("PASS exact enum tag/identity typed MessagePack v2 vector");
    Ok(())
}


fn native_async_io_boundary(compiler: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-async-io-boundary-").map_err(|e| e.to_string())?;
    let fixture = temp.path.join("probe.ores");

    // Concrete standard-I/O spellings named by the native async-only contract.
    // Until registry/effect/continuation lowering lands together, each spelling
    // must be unavailable as a call and impossible to impersonate as a user
    // global function or method.
    let operations = [
        ("legacy read", "read_text"),
        ("legacy write", "write_text"),
        ("read blocking", "read_text_blocking"),
        ("read async", "read_text_async"),
        ("write blocking", "write_text_blocking"),
        ("write async", "write_text_async"),
        ("TCP connect blocking", "connect_blocking"),
        ("TCP connect async", "connect_async"),
        ("TCP read blocking", "read_line_blocking"),
        ("TCP read async", "read_line_async"),
        ("TCP write blocking", "socket_write_text_blocking"),
        ("TCP write async", "socket_write_text_async"),
        ("HTTP GET blocking", "http_get_text_blocking"),
        ("HTTP GET async", "http_get_text_async"),
        ("HTTP POST blocking", "http_post_text_blocking"),
        ("HTTP POST async", "http_post_text_async"),
        ("stdio blocking", "println_blocking"),
        ("stdio async", "println_async"),
    ];

    for (name, symbol) in operations {
        let undefined_call = format!(
            r#"pub fnc main(): int {{ return {symbol}(1); }}
"#
        );
        fs::write(&fixture, undefined_call)
            .map_err(|e| format!("{name}: write undefined-call fixture: {e}"))?;
        let out = run(compiler, [fixture.as_os_str()])?;
        require(
            !out.status.success(),
            format!("{name}: unsupported native I/O compiled: {}", stdout(&out)),
        )?;
        require(
            stdout(&out).trim().is_empty(),
            format!("{name}: unsupported native I/O emitted LLVM IR: {}", stdout(&out)),
        )?;
        require(
            stderr(&out).contains("unknown function or wrong call arity"),
            format!("{name}: missing undefined-call rejection diagnostic: {}", stderr(&out)),
        )?;

        let global_launder = format!(
            r#"fnc {symbol}(int n): int {{ return n; }}
pub fnc main(): int {{ return {symbol}(1); }}
"#
        );
        fs::write(&fixture, global_launder)
            .map_err(|e| format!("{name}: write declaration-laundering fixture: {e}"))?;
        let out = run(compiler, [fixture.as_os_str()])?;
        require(
            !out.status.success()
                && stdout(&out).trim().is_empty()
                && stderr(&out).contains("native I/O callable name is compiler-owned"),
            format!(
                "{name}: global declaration laundered reserved I/O identity; status={:?}, stdout={}, stderr={}",
                out.status.code(),
                stdout(&out),
                stderr(&out)
            ),
        )?;

        let method_launder = format!(
            r#"define class SocketFacade as
  pub {symbol}(self, int n): int {{ return n; }}
end
pub fnc main(): int {{
  let SocketFacade s = new SocketFacade();
  return s.{symbol}(1);
}}
"#
        );
        fs::write(&fixture, method_launder)
            .map_err(|e| format!("{name}: write method-laundering fixture: {e}"))?;
        let out = run(compiler, [fixture.as_os_str()])?;
        require(
            !out.status.success()
                && stdout(&out).trim().is_empty()
                && stderr(&out).contains("native I/O callable name is compiler-owned"),
            format!(
                "{name}: method declaration laundered reserved I/O identity; status={:?}, stdout={}, stderr={}",
                out.status.code(),
                stdout(&out),
                stderr(&out)
            ),
        )?;
    }

    // The reservation is identity-based, not suffix-based. Preserve ordinary
    // synchronous user functions that happen to end in _async/_blocking.
    let positives = [
        ("blocking suffix", "pure_math_blocking"),
        ("async suffix", "checksum_async"),
        ("similar I/O-looking suffix", "read_textish_async"),
    ];
    for (name, symbol) in positives {
        let program = format!(
            r#"fnc {symbol}(int n): int {{ return n + 1; }}
pub fnc main(): int {{ return {symbol}(41); }}
"#
        );
        fs::write(&fixture, program)
            .map_err(|e| format!("{name}: write positive fixture: {e}"))?;
        let out = run(compiler, [fixture.as_os_str()])?;
        require(
            out.status.success() && stdout(&out).starts_with("; ModuleID"),
            format!(
                "{name}: ordinary user suffix was over-reserved; status={:?}, stdout={}, stderr={}",
                out.status.code(),
                stdout(&out),
                stderr(&out)
            ),
        )?;
    }

    println!(
        "Native async-only I/O boundary: {} reserved standard spellings reject undefined/global/method laundering; {} user suffix controls compile",
        operations.len(),
        positives.len()
    );
    Ok(())
}

// Executable source-to-AOT proof for receiver-erased method selectors. The
// native frontend stores a hidden receiver in its ABI, not source arity.
fn native_inline_self_arity(compiler: &Path, clang: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-inline-self-").map_err(|e| e.to_string())?;
    let source = temp.path.join("methods.ores");
    let ir = temp.path.join("methods.ll");
    let executable = temp.path.join("methods-native");

    let positives = [
        (
            "exact-empty-void",
            "define class X1 as\n  foo():void {}\nend\n\
             define class X2 as\n  foo(self):void {}\nend\n\
             pub fnc main():int { return 42; }\n",
        ),
        (
            "public-void-dispatch",
            "define class X1 as\n  pub foo():void {}\nend\n\
             define class X2 as\n  pub foo(self):void {}\nend\n\
             pub fnc main():int {\n  let X1 x1=new X1();\n  let X2 x2=new X2();\n  x1.foo();\n  x2.foo();\n  return 42;\n}\n",
        ),
        (
            "mixed-method-arities",
            "define class Counter as\n  let int value=20;\n\
             pub read():int{return self.value;}\n\
             pub read(self, int n):int{return self.value+n;}\nend\n\
             pub fnc main():int {let Counter c=new Counter();return c.read()+c.read(2);}\n",
        ),
        (
            "mtd-optional-receiver-arity",
            "define class X as\n  pub mtd read(self):int{return 42;}\n\
             pub read(self,int n):int{return n;}\nend\n\
             pub fnc main():int {let X x=new X();return x.read()+x.read(0);}\n",
        ),
        (
            "mtd-arrow-end-delimited",
            "define class X as\n  pub mtd zero(self) -> void ::\n  end\n\
             pub mtd answer(self) -> int ::\n    return 42;\n  end\n\
             pub mtd echo(self,int value) -> int ::\n    return value;\n  end\n\
             end\n\
             pub fnc main():int {let X x=new X();x.zero();return x.answer()+x.echo(0);}\n",
        ),
        (
            "mutable-inline-self",
            "define class Counter as\n  let int value=40;\n\
             pub bump(mut self, int n):void{self.value=self.value+n;return;}\n\
             pub read(self):int{return self.value;}\nend\n\
             pub fnc main():int {let mut Counter c=new Counter();c.bump(2);return c.read();}\n",
        ),
    ];
    for (name, program) in positives {
        fs::write(&source, program).map_err(|e| e.to_string())?;
        let out = run(compiler, [source.as_os_str()])?;
        require(
            out.status.success() && stdout(&out).starts_with("; ModuleID"),
            format!("{name}: compiler rejected source or did not emit IR: {}", stderr(&out)),
        )?;
        if name == "mixed-method-arities" {
            let symbols = stdout(&out);
            require(
                symbols.contains("Counter.read$arity0")
                    && symbols.contains("Counter.read$arity1")
                    && !symbols.contains("Counter.read$arity2"),
                "overloaded method symbols must use caller-visible 0/1 arity, not hidden-ABI 1/2",
            )?;
        }
        fs::write(&ir, &out.stdout).map_err(|e| e.to_string())?;
        let linked = run(
            clang,
            [
                OsStr::new("-O1"),
                ir.as_os_str(),
                OsStr::new("-o"),
                executable.as_os_str(),
            ],
        )?;
        require(
            linked.status.success(),
            format!("{name}: clang failed: {}", stderr(&linked)),
        )?;
        let executed = run(&executable, [] as [&OsStr; 0])?;
        require(
            executed.status.code() == Some(42),
            format!("{name}: native return status {:?}, stderr={}", executed.status.code(), stderr(&executed)),
        )?;
    }

    let negatives = [
        (
            "duplicate-zero-slot",
            "define class X as foo():void {} foo(self):void {} end\n\
             pub fnc main():int{return 42;}\n",
            "duplicate native method name/arity",
        ),
        (
            "duplicate-one-slot",
            "define class X as foo(int n):void {} foo(self,int n):void {} end\n\
             pub fnc main():int{return 42;}\n",
            "duplicate native method name/arity",
        ),
        (
            "explicit-call-site-receiver-is-not-erased",
            "define class X as\n  pub foo(self,int n):int{return n;}\n\
             pub wrong(self):int{return self.foo(self,5);}\nend\n\
             pub fnc main():int {let X x=new X();return x.wrong();}\n",
            "unknown method or wrong explicit call arity 'foo'",
        ),
        (
            "mtd-duplicate-zero-slot",
            "define class X as mtd foo():void {} foo(self):void {} end\n\
             pub fnc main():int{return 42;}\n",
            "duplicate native method name/arity",
        ),
        (
            "mtd-without-method-signature",
            "define class X as mtd int x=3; end\n\
             pub fnc main():int{return 42;}\n",
            "native mtd marker must precede a method signature",
        ),
        (
            "end-delimited-colon-must-be-arrow",
            "define class X as mtd foo():void :: end end\n\
             pub fnc main():int{return 42;}\n",
            "native '::' body requires '->' return syntax",
        ),
        (
            "end-delimited-missing-double-colon",
            "define class X as mtd foo(self) -> int return 42; end end\n\
             pub fnc main():int{return 42;}\n",
            "expected '{'",
        ),
        (
            "end-delimited-as-is-not-double-colon",
            "define class X as mtd foo(self) -> int as return 42; end end\n\
             pub fnc main():int{return 42;}\n",
            "expected '{'",
        ),
        (
            "end-delimited-nontrivial-void-body",
            "define class X as mtd foo() -> void :: stdio.println(42); end end\n\
             pub fnc main():int{return 42;}\n",
            "native comma expression body requires a single terminal result",
        ),
        (
            "end-delimited-nonvoid-missing-return",
            "define class X as mtd foo() -> int :: end end\n\
             pub fnc main():int{return 42;}\n",
            "native non-void comma expression body requires a result",
        ),
        (
            "trailing-receiver-comma",
            "define class X as foo(self,):void {} end\n\
             pub fnc main():int{return 42;}\n",
            "explicit self receiver must be followed by a parameter",
        ),
        (
            "typed-read-receiver-unsupported",
            "define class X as foo(self X):void {} end\n\
             pub fnc main():int{return 42;}\n",
            "explicitly typed read self receiver unsupported",
        ),
        (
            "duplicate-mutable-self",
            "define class X as foo():void {} foo(mut self):void {} end\n\
             pub fnc main():int{return 42;}\n",
            "duplicate native method name/arity",
        ),
    ];
    for (name, program, diagnostic) in negatives {
        fs::write(&source, program).map_err(|e| e.to_string())?;
        let out = run(compiler, [source.as_os_str()])?;
        require(
            !out.status.success()
                && stdout(&out).trim().is_empty()
                && stderr(&out).contains(diagnostic),
            format!("{name}: expected rejection '{diagnostic}'; status={:?}, stdout={}, stderr={}",
                out.status.code(), stdout(&out), stderr(&out)),
        )?;
    }
    println!("native inline self: 6 compiled/AOT-executed positive cases, 13 fail-closed negatives");
    Ok(())
}

// Compiler -> LLVM IR -> Clang -> actual process. This deliberately closes
// only the safe int % nonzero signed-literal subset; dynamic and zero divisors
// remain rejected until Oreslang's checked failure/effects ABI is available.
fn native_remainder_parity(compiler: &Path, clang: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-remainder-parity-").map_err(|e| e.to_string())?;
    let source = temp.path.join("remainder.ores");
    let ir = temp.path.join("remainder.ll");
    let executable = temp.path.join("remainder-native");

    // % and single ^ are reserved (^^ is bool XOR). Named Integer operations carry the semantics:
    // rem truncates toward zero, mod is never negative, and a zero divisor traps.
    let min = "(-9223372036854775807 - 1)";
    let rem_fn = "fnc r(int a, int b): int { return Integer.rem(a, b); }\n";
    let mod_fn = "fnc m(int a, int b): int { return Integer.mod(a, b); }\n";
    let positives: Vec<(&str, String, i32)> = vec![
        ("positive", "pub fnc main(): int { return Integer.rem(17, 5); }\n".into(), 2),
        ("negative-dividend", "pub fnc main(): int { return Integer.rem(-17, 5) + 10; }\n".into(), 8),
        ("precedence", "pub fnc main(): int { return 4 + Integer.rem(17, 5) * 3; }\n".into(), 10),
        ("nested", "pub fnc main(): int { return Integer.rem(Integer.rem(20, 6), 4); }\n".into(), 2),
        ("function-param", "fnc rem(int n): int { return Integer.rem(n, 7); }\n\
            pub fnc main(): int { return rem(52); }\n".into(), 3),
        ("exact-divisor", "pub fnc main(): int { return Integer.rem(64, 8); }\n".into(), 0),
        ("divisor-one", "pub fnc main(): int { return Integer.rem(-14, 1) + 6; }\n".into(), 6),
        ("signed-min-divisor-one", format!("pub fnc main(): int {{ return Integer.rem({min}, 1) + 11; }}\n"), 11),
        ("negative-divisor", "pub fnc main(): int { return Integer.rem(17, -5); }\n".into(), 2),
        ("both-negative", "pub fnc main(): int { return Integer.rem(-17, -5) + 10; }\n".into(), 8),
        ("negative-minus-one", "pub fnc main(): int { return Integer.rem(5, -1); }\n".into(), 0),
        ("min-dividend-minus-one", format!("pub fnc main(): int {{ return Integer.rem({min}, -1) + 13; }}\n"), 13),
        ("signed-min-divisor", format!("pub fnc main(): int {{ return Integer.rem(5, {min}); }}\n"), 5),
        ("min-both-sides", format!("pub fnc main(): int {{ return Integer.rem({min}, {min}) + 17; }}\n"), 17),
        // Full signed-i64 equality returns 0/1; POSIX process exit status
        // otherwise truncates an incorrect large result modulo 256.
        ("full-width-min-negative-seven", format!("pub fnc main(): int {{ return (Integer.rem({min}, -7) == -1) ? 0 : 1; }}\n"), 0),
        ("full-width-max-negative-seven", "pub fnc main(): int { return (Integer.rem(9223372036854775807, -7) == 0) ? 0 : 1; }\n".into(), 0),
        ("runtime-divisor", format!("{rem_fn}pub fnc main(): int {{ return r(52, 7); }}\n"), 3),
        ("runtime-negative-divisor", format!("{rem_fn}pub fnc main(): int {{ return r(-17, -5) + 10; }}\n"), 8),
        ("runtime-min-minus-one", format!("{rem_fn}pub fnc main(): int {{ return r({min}, -1) + 21; }}\n"), 21),
        ("mod-negative-dividend", "pub fnc main(): int { return Integer.mod(-9, 4); }\n".into(), 3),
        ("mod-both-negative", "pub fnc main(): int { return Integer.mod(-9, -4); }\n".into(), 3),
        ("mod-negative-divisor", "pub fnc main(): int { return Integer.mod(9, -4); }\n".into(), 1),
        ("mod-runtime", format!("{mod_fn}pub fnc main(): int {{ return m(-9, 4); }}\n"), 3),
        ("mod-runtime-min-minus-one", format!("{mod_fn}pub fnc main(): int {{ return m({min}, -1) + 5; }}\n"), 5),
        ("mod-full-width", format!("{mod_fn}pub fnc main(): int {{ return (m(-1, {min}) == 9223372036854775807) ? 0 : 1; }}\n"), 0),
        ("bitxor", "pub fnc main(): int { return Integer.bitXor(5, 3); }\n".into(), 6),
        ("bitxor-full-width", format!("pub fnc main(): int {{ return (Integer.bitXor({min}, 9223372036854775807) == -1) ? 0 : 1; }}\n"), 0),
        ("bool-xor", "pub fnc main(): int { return (Xor(true, false) && !Xor(true, true)) ? 0 : 1; }\n".into(), 0),
    ];
    for (name, program, expected) in &positives {
        let (name, expected) = (*name, *expected);
        fs::write(&source, program).map_err(|e| format!("{name}: write: {e}"))?;
        let compiled = run(compiler, [source.as_os_str()])?;
        require(
            compiled.status.success() && stdout(&compiled).starts_with("; ModuleID"),
            format!("{name}: native frontend rejected source: {}", stderr(&compiled)),
        )?;
        let llvm_ir = stdout(&compiled);
        if name == "function-param" || name == "runtime-divisor" {
            require(
                llvm_ir.contains("srem i64 "),
                format!("{name}: emitted IR lacks runtime signed remainder"),
            )?;
        }
        if name == "runtime-min-minus-one" {
            require(
                llvm_ir.contains("rem.divisor"),
                format!("{name}: missing the -1 divisor guard that avoids srem poison"),
            )?;
        }
        fs::write(&ir, &compiled.stdout).map_err(|e| format!("{name}: IR: {e}"))?;
        for level in ["-O0", "-O2"] {
            let linked = run(
                clang,
                [
                    OsStr::new(level),
                    ir.as_os_str(),
                    OsStr::new("-o"),
                    executable.as_os_str(),
                ],
            )?;
            require(
                linked.status.success(),
                format!("{name} {level}: Clang failed: {}", stderr(&linked)),
            )?;
            let executed = run(&executable, std::iter::empty::<&str>())?;
            require(
                executed.status.code() == Some(expected)
                    && executed.stdout.is_empty()
                    && executed.stderr.is_empty(),
                format!("{name} {level}: observed {:?}, expected {expected}",
                    executed.status.code()),
            )?;
        }
    }
    let traps: Vec<(&str, String)> = vec![
        ("zero-literal-divisor", "pub fnc main(): int { return Integer.rem(5, 0); }\n".into()),
        ("zero-runtime-divisor", format!("{rem_fn}pub fnc main(): int {{ return r(5, 0); }}\n")),
        ("mod-zero-runtime-divisor", format!("{mod_fn}pub fnc main(): int {{ return m(5, 0); }}\n")),
    ];
    for (name, program) in &traps {
        fs::write(&source, program).map_err(|e| format!("{name}: write: {e}"))?;
        let compiled = run(compiler, [source.as_os_str()])?;
        require(compiled.status.success(),
            format!("{name}: native frontend rejected source: {}", stderr(&compiled)))?;
        fs::write(&ir, &compiled.stdout).map_err(|e| format!("{name}: IR: {e}"))?;
        for level in ["-O0", "-O2"] {
            let linked = run(clang, [OsStr::new(level), ir.as_os_str(), OsStr::new("-o"), executable.as_os_str()])?;
            require(linked.status.success(), format!("{name} {level}: Clang failed: {}", stderr(&linked)))?;
            let executed = run(&executable, std::iter::empty::<&str>())?;
            // A zero divisor must stop the program, never return a value.
            require(executed.status.code().is_none(),
                format!("{name} {level}: expected a trap, observed exit {:?}", executed.status.code()))?;
        }
    }
    let negatives = [
        ("percent", "pub fnc main(): int { return 17 % 5; }\n", "'%' is reserved"),
        ("caret", "pub fnc main(): int { return 5 ^ 3; }\n", "'^' is reserved"),
        // `^^` is canonical bool XOR (spec ch.16); int operands are a type error.
        ("double-caret-int", "pub fnc main(): int { return 7 ^^ 3; }\n", "logical operator requires bool"),
        ("pow-unsupported", "pub fnc main(): int { return Integer.pow(2, 3); }\n", "Integer.pow is unsupported"),
        ("wrong-type", "pub fnc main(): int { return Integer.rem(true, 2); }\n", "arithmetic requires int"),
        ("xor-shadowing", "fnc Xor(bool a, bool b): bool { return a; }\npub fnc main(): int { return 0; }\n", "expected non-keyword identifier"),
    ];
    for (name, program, diagnostic) in negatives {
        fs::write(&source, program).map_err(|e| format!("{name}: write: {e}"))?;
        let rejected = run(compiler, [source.as_os_str()])?;
        require(
            !rejected.status.success()
                && rejected.stdout.is_empty()
                && stderr(&rejected).contains(diagnostic),
            format!("{name}: missing fail-closed diagnostic {diagnostic:?}: {}",
                stderr(&rejected)),
        )?;
    }
    println!(
        "Native Integer.rem/mod/bitXor: {} compiled cases and {} zero-divisor traps at -O0/-O2, {} reserved-operator negatives",
        positives.len(), traps.len(), negatives.len()
    );
    Ok(())
}

fn path_arg(args: &[String], index: usize, usage: &str) -> Result<PathBuf, String> {
    args.get(index)
        .map(PathBuf::from)
        .ok_or_else(|| usage.to_string())
}

fn real_main() -> Result<(), String> {
    let args: Vec<String> = env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("checked-i64") => {
            let compiler = path_arg(&args, 2, "usage: checked-i64 <compiler> <clang>")?;
            let clang = path_arg(&args, 3, "usage: checked-i64 <compiler> <clang>")?;
            checked_i64::run_cases(&compiler, &clang)
        }
        Some("realtime-profile") => {
            let compiler = path_arg(&args, 2, "usage: realtime-profile <compiler>")?;
            realtime_profile(&compiler)
        }
        Some("nominal-type-binders") => {
            let compiler = path_arg(&args, 2, "usage: nominal-type-binders <compiler>")?;
            nominal_type_binders(&compiler)
        }
        Some("callable-nominal") => {
            let compiler = path_arg(&args, 2, "usage: callable-nominal <compiler>")?;
            callable_nominal(&compiler)
        }
        Some("native-inline-self-arity") => {
            let compiler = path_arg(&args, 2, "usage: native-inline-self-arity <compiler> <clang>")?;
            let clang = path_arg(&args, 3, "usage: native-inline-self-arity <compiler> <clang>")?;
            native_inline_self_arity(&compiler, &clang)
        }
        Some("native-async-io-boundary") => {
            let compiler = path_arg(&args, 2, "usage: native-async-io-boundary <compiler>")?;
            native_async_io_boundary(&compiler)
        }
        Some("native-remainder-parity") => {
            let compiler = path_arg(&args, 2, "usage: native-remainder-parity <compiler> <clang>")?;
            let clang = path_arg(&args, 3, "usage: native-remainder-parity <compiler> <clang>")?;
            native_remainder_parity(&compiler, &clang)
        }
        Some("named-call-order") => {
            let compiler = path_arg(&args, 2, "usage: named-call-order <compiler> <fixture>")?;
            let fixture = path_arg(&args, 3, "usage: named-call-order <compiler> <fixture>")?;
            named_call_source_order(&compiler, &fixture)
        }
        Some("actor-persistent-fields") => {
            let compiler = path_arg(&args, 2, "usage: actor-persistent-fields <compiler> <clang> <root>")?;
            let clang = path_arg(&args, 3, "usage: actor-persistent-fields <compiler> <clang> <root>")?;
            let root = path_arg(&args, 4, "usage: actor-persistent-fields <compiler> <clang> <root>")?;
            actor_persistent_fields(&compiler, &clang, &root)
        }
        Some("tagged-message-wire") => {
            let binary = path_arg(&args, 2, "usage: tagged-message-wire <binary> <vectors>")?;
            let vectors = path_arg(&args, 3, "usage: tagged-message-wire <binary> <vectors>")?;
            tagged_message_wire(&binary, &vectors)
        }
        Some("aot-artifact-header-selftest") => aot_artifact_header_selftest(),
        Some("aot-driver") => {
            let driver = path_arg(&args, 2, "usage: aot-driver <driver> <compiler> <clang>")?;
            let compiler = path_arg(&args, 3, "usage: aot-driver <driver> <compiler> <clang>")?;
            let clang = path_arg(&args, 4, "usage: aot-driver <driver> <compiler> <clang>")?;
            aot_driver(&driver, &compiler, &clang)
        }
        _ => Err(
            "usage: oreslang-llvm-test-harness <realtime-profile|callable-nominal|nominal-type-binders|aot-driver|native-remainder-parity|named-call-order|actor-persistent-fields|tagged-message-wire|native-async-io-boundary|native-inline-self-arity> ..."
                .to_string(),
        ),
    }
}

fn main() {
    if let Err(error) = real_main() {
        eprintln!("FAIL: {error}");
        std::process::exit(1);
    }
}

