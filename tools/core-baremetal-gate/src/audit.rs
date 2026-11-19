//! Pure audit rules for a freestanding `ores.core` image.
//!
//! Everything here works on tool output text (LLVM IR, `llvm-nm`,
//! `llvm-readelf`) so the rules are unit-testable without a toolchain.

/// Compiler-support helpers an `ores.core` object may ask the platform for.
///
/// This is the whole freestanding support ABI: memory primitives and the ARM
/// run-time ABI (RTABI) arithmetic helpers that LLVM may emit for 64-bit
/// operations on 32-bit cores. None of them is libc, an OS service, an
/// allocator or a runtime. A guest requesting anything else fails the gate.
pub const SUPPORT_ABI: &[&str] = &[
    "memcpy",
    "memmove",
    "memset",
    "memcmp",
    "__aeabi_memcpy",
    "__aeabi_memcpy4",
    "__aeabi_memcpy8",
    "__aeabi_memmove",
    "__aeabi_memmove4",
    "__aeabi_memmove8",
    "__aeabi_memset",
    "__aeabi_memset4",
    "__aeabi_memset8",
    "__aeabi_memclr",
    "__aeabi_memclr4",
    "__aeabi_memclr8",
    "__aeabi_ldivmod",
    "__aeabi_uldivmod",
    "__aeabi_idivmod",
    "__aeabi_uidivmod",
    "__aeabi_idiv",
    "__aeabi_uidiv",
    "__aeabi_lmul",
    "__aeabi_llsl",
    "__aeabi_llsr",
    "__aeabi_lasr",
    "__mulodi4",
];

/// Exact host, libc, allocator and threading symbols. Each gets a specific
/// diagnostic even though the allowlist would reject it anyway.
pub const FORBIDDEN: &[&str] = &[
    // allocator
    "malloc", "calloc", "realloc", "free", "aligned_alloc", "posix_memalign",
    "memalign", "valloc", "pvalloc", "sbrk", "_sbrk", "brk", "mmap", "munmap",
    // stdio / file descriptors
    "printf", "fprintf", "sprintf", "snprintf", "vprintf", "puts", "putchar",
    "fputs", "fwrite", "fflush", "write", "read", "open", "close", "lseek",
    "fopen", "fclose", "_write", "_read", "_open", "_close", "_lseek", "_fstat",
    "_isatty",
    // process / signals
    "exit", "_exit", "abort", "atexit", "raise", "signal", "kill", "getpid",
    "_getpid", "_kill",
    // environment / time
    "getenv", "setenv", "time", "clock_gettime", "gettimeofday", "nanosleep",
    "sleep", "usleep",
    // errno / TLS
    "__errno", "__errno_location", "__tls_get_addr", "__aeabi_read_tp",
];

/// Symbol families that imply a hosted runtime, unwinder or Oreslang service.
pub const FORBIDDEN_PREFIXES: &[(&str, &str)] = &[
    ("pthread_", "OS threads"),
    ("_Unwind_", "unwinder"),
    ("__cxa_", "C++ runtime"),
    ("__gxx_personality", "C++ exception personality"),
    ("__aeabi_unwind_cpp_", "unwinder personality"),
    ("ores_actor_", "actor runtime"),
    ("oreslang_actor_", "actor runtime"),
    ("oreslang_rt_", "Oreslang runtime service"),
    ("ores_string_", "heap string runtime"),
    ("ores_record_", "heap record runtime"),
    ("oreslang_owned_", "owned-region runtime"),
    ("oreslang_int_", "array region runtime"),
    ("oreslang_source_", "source runtime bootstrap"),
    ("oreslang_freestanding_", "freestanding provider runtime"),
    ("oreslang_cortexm_", "Cortex-M actor bootstrap"),
    ("oreslang_wasm_", "Wasm host runtime"),
];

/// Program header types a static freestanding image must not carry.
pub const FORBIDDEN_SEGMENTS: &[&str] = &["INTERP", "DYNAMIC", "TLS", "GNU_RELRO"];

/// Section names that imply a dynamic loader, constructors or TLS.
pub const FORBIDDEN_SECTIONS: &[&str] = &[
    ".interp", ".dynamic", ".dynsym", ".dynstr", ".plt", ".got.plt",
    ".init_array", ".fini_array", ".preinit_array", ".ctors", ".dtors",
    ".tdata", ".tbss",
];

/// Why a symbol is not allowed, or `None` if it is in neither denylist.
pub fn forbidden_reason(symbol: &str) -> Option<&'static str> {
    if FORBIDDEN.contains(&symbol) {
        return Some("libc/OS/allocator symbol");
    }
    FORBIDDEN_PREFIXES
        .iter()
        .find(|(prefix, _)| symbol.starts_with(prefix))
        .map(|(_, reason)| *reason)
}

/// Names of functions the IR declares but does not define, excluding LLVM
/// intrinsics. A provider-free `ores.core` module should have none.
pub fn ir_external_declarations(ir: &str) -> Vec<String> {
    ir.lines()
        .map(str::trim_start)
        .filter(|line| line.starts_with("declare "))
        .filter_map(declared_name)
        .filter(|name| !name.starts_with("llvm."))
        .collect()
}

/// Ways IR can reach the host without a declared function: external globals
/// (`@stdout = external global ...`), module-level `asm`, and inline `asm`
/// calls (an `svc`/`bkpt` syscall bypasses every symbol audit).
pub fn ir_escape_hatches(ir: &str) -> Vec<String> {
    let mut found = Vec::new();
    for line in ir.lines().map(str::trim) {
        if line.starts_with(';') {
            continue;
        }
        if line.starts_with("module asm") {
            found.push(format!("module-level assembly: {line}"));
        } else if line.contains(" asm ") || line.contains(" asm sideeffect ") {
            found.push(format!("inline assembly: {line}"));
        } else if is_external_global_declaration(line) {
            found.push(format!("external global: {line}"));
        }
    }
    found
}

/// LLVM IR permits qualifiers between the linkage and the global kind, e.g.
/// \`@g = external thread_local addrspace(1) global i32\`. Do not infer
/// externality from a fixed substring: an external global may carry TLS,
/// address-space, visibility or other modifiers.
fn is_external_global_declaration(line: &str) -> bool {
    let Some((name, rest)) = line.split_once('=') else {
        return false;
    };
    if !name.trim().starts_with('@') {
        return false;
    }
    let mut external = false;
    for word in rest.split_whitespace() {
        if matches!(word, "external" | "extern_weak") {
            external = true;
        }
        if matches!(word, "global" | "constant") {
            return external;
        }
    }
    false
}

fn declared_name(line: &str) -> Option<String> {
    let at = line.find('@')?;
    let rest = &line[at + 1..];
    if let Some(quoted) = rest.strip_prefix('"') {
        return quoted.find('"').map(|end| quoted[..end].to_string());
    }
    let end = rest
        .find(|c: char| !(c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '$')))
        .unwrap_or(rest.len());
    (end > 0).then(|| rest[..end].to_string())
}

/// One `llvm-nm` row: `kind` is the nm type letter (`U` for undefined).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NmSymbol {
    pub kind: char,
    pub name: String,
}

/// Parse default-format `llvm-nm` output for a single file.
pub fn parse_nm(output: &str) -> Vec<NmSymbol> {
    output
        .lines()
        .filter(|line| !line.trim().is_empty() && !line.trim_end().ends_with(':'))
        .filter_map(|line| {
            let mut fields = line.split_whitespace().rev();
            let name = fields.next()?;
            let kind = fields.next()?.chars().next()?;
            Some(NmSymbol { kind, name: name.to_string() })
        })
        .collect()
}

/// Audit the undefined symbols of a compiled guest object.
pub fn audit_guest_object(symbols: &[NmSymbol]) -> Vec<String> {
    symbols
        .iter()
        // `w`/`v` are weak undefined references: still a request for a host symbol.
        .filter(|s| matches!(s.kind, 'U' | 'w' | 'v'))
        .filter_map(|s| {
            if let Some(reason) = forbidden_reason(&s.name) {
                Some(format!("forbidden {reason} symbol `{}`", s.name))
            } else if !SUPPORT_ABI.contains(&s.name.as_str()) {
                Some(format!(
                    "symbol `{}` is outside the ores.core freestanding support ABI",
                    s.name
                ))
            } else {
                None
            }
        })
        .collect()
}

/// Index of the first console line containing `needle`, if any.
pub fn line_index(console: &str, needle: &str) -> Option<usize> {
    console.lines().position(|line| line.contains(needle))
}

/// Audit a fully linked image: nothing undefined, and no forbidden symbol
/// even as a definition (a linked-in malloc is still a heap).
pub fn audit_linked_image(symbols: &[NmSymbol]) -> Vec<String> {
    let mut violations = Vec::new();
    for s in symbols {
        if s.kind == 'U' || s.kind == 'w' || s.kind == 'v' {
            violations.push(format!("linked image leaves `{}` unresolved", s.name));
        } else if let Some(reason) = forbidden_reason(&s.name) {
            violations.push(format!("linked image contains {reason} symbol `{}`", s.name));
        }
    }
    violations
}

/// Audit `llvm-readelf -l` output for loader/TLS segments.
pub fn audit_program_headers(readelf: &str) -> Vec<String> {
    readelf
        .lines()
        .filter_map(|line| line.split_whitespace().next())
        .filter(|kind| FORBIDDEN_SEGMENTS.contains(kind))
        .map(|kind| format!("linked image has a {kind} program header"))
        .collect()
}

/// Audit `llvm-readelf -S` output for loader, constructor or TLS sections.
pub fn audit_sections(readelf: &str) -> Vec<String> {
    readelf
        .lines()
        .filter_map(|line| {
            let after = line.split_once(']')?.1;
            after.split_whitespace().next()
        })
        .filter(|name| {
            FORBIDDEN_SECTIONS
                .iter()
                .any(|bad| *name == *bad || name.starts_with(&format!("{bad}.")))
        })
        .map(|name| format!("linked image has forbidden section `{name}`"))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sym(kind: char, name: &str) -> NmSymbol {
        NmSymbol { kind, name: name.to_string() }
    }

    #[test]
    fn ir_declarations_ignore_intrinsics_and_definitions() {
        let ir = r#"
define i64 @entry(i64 %x) {
declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64) #0
declare void @llvm.trap() #1
declare ptr @malloc(i64)
declare void @"ores_actor_send"(ptr)
"#;
        assert_eq!(ir_external_declarations(ir), vec!["malloc", "ores_actor_send"]);
    }

    #[test]
    fn nm_parsing_handles_undefined_and_defined_rows() {
        let out = "guest.o:\n         U memcpy\n00000038 T sum\n\n";
        assert_eq!(parse_nm(out), vec![sym('U', "memcpy"), sym('T', "sum")]);
    }

    #[test]
    fn guest_may_request_only_support_abi_helpers() {
        let ok = [sym('U', "__aeabi_uldivmod"), sym('U', "memset"), sym('T', "entry")];
        assert!(audit_guest_object(&ok).is_empty());

        let bad = [
            sym('U', "malloc"),
            sym('U', "pthread_create"),
            sym('U', "ores_actor_spawn"),
            sym('U', "mystery_helper"),
        ];
        let v = audit_guest_object(&bad);
        assert_eq!(v.len(), 4, "{v:?}");
        assert!(v[0].contains("libc/OS/allocator"));
        assert!(v[1].contains("OS threads"));
        assert!(v[2].contains("actor runtime"));
        assert!(v[3].contains("outside the ores.core freestanding support ABI"));
    }

    #[test]
    fn linked_image_rejects_unresolved_and_linked_in_allocators() {
        assert!(audit_linked_image(&[sym('T', "Reset_Handler")]).is_empty());
        let v = audit_linked_image(&[sym('U', "memcpy"), sym('T', "malloc"), sym('w', "x")]);
        assert_eq!(v.len(), 3, "{v:?}");
    }

    #[test]
    fn program_headers_reject_dynamic_and_tls() {
        let ok = "  LOAD 0x010000 0x00000000 0x00000000 0x00040 0x00040 R 0x10000\n";
        assert!(audit_program_headers(ok).is_empty());
        let bad = "  INTERP 0x1 0x2\n  TLS 0x1 0x2\n  LOAD 0x1\n";
        assert_eq!(audit_program_headers(bad).len(), 2);
    }

    #[test]
    fn sections_reject_constructors_and_tls() {
        let ok = "  [ 1] .isr_vector PROGBITS\n  [ 2] .text PROGBITS\n";
        assert!(audit_sections(ok).is_empty());
        let bad = "  [ 3] .init_array INIT_ARRAY\n  [ 4] .tbss NOBITS\n  [ 5] .tdata.x PROGBITS\n";
        assert_eq!(audit_sections(bad).len(), 3);
    }

    #[test]
    fn ir_escape_hatches_catch_globals_and_assembly() {
        let clean = "define i64 @f(i64 %x) {\n  ret i64 %x\n}\n; comment mentioning asm \n";
        assert!(ir_escape_hatches(clean).is_empty());
        let dirty = "module asm \"svc 0\"\n@stdout = external global ptr\n@w = extern_weak global i8\n  call void asm sideeffect \"svc #0\", \"\"()\n";
        let found = ir_escape_hatches(dirty);
        assert_eq!(found.len(), 4, "{found:?}");
        assert!(found[0].starts_with("module-level assembly"));
        assert!(found[1].starts_with("external global"));
        assert!(found[3].starts_with("inline assembly"));
    }

    #[test]
    fn ir_external_global_modifiers_cannot_bypass_gate() {
        let variants = [
            "@tls = external thread_local global i32",
            "@tls_weak = extern_weak thread_local(localexec) global i64",
            "@as = external addrspace(3) global i8",
            "@const = external dso_local unnamed_addr constant i32",
            "@weak_const = extern_weak hidden constant i32",
        ];
        for ir in variants {
            let errors = ir_escape_hatches(ir);
            assert_eq!(errors.len(), 1, "{ir}: {errors:?}");
            assert!(errors[0].starts_with("external global:"), "{ir}: {errors:?}");
        }
        // Fully defined globals, including the similarly named internal
        // \`externally_initialized\` modifier, are not host references.
        assert!(ir_escape_hatches("@counter = global i32 0").is_empty());
        assert!(ir_escape_hatches("@counter = externally_initialized global i32 0").is_empty());
    }

    #[test]
    fn guest_weak_undefined_references_are_audited() {
        let v = audit_guest_object(&[sym('w', "malloc"), sym('v', "mystery")]);
        assert_eq!(v.len(), 2, "{v:?}");
        assert!(v[0].contains("libc/OS/allocator"));
    }

    #[test]
    fn console_line_order_is_checkable() {
        let out = "ORES_CORE_TRAP_PROBE start\nORES_CORE_TRAP hardfault\n";
        assert_eq!(line_index(out, "PROBE start"), Some(0));
        assert_eq!(line_index(out, "hardfault"), Some(1));
        assert_eq!(line_index(out, "missing"), None);
    }

    #[test]
    fn support_abi_and_denylists_do_not_overlap() {
        for helper in SUPPORT_ABI {
            assert!(forbidden_reason(helper).is_none(), "{helper} is both allowed and forbidden");
        }
    }
}
