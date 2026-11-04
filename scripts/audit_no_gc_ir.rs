//! Fail closed on known tracing-GC LLVM IR in the standalone native AOT path.
//!
//! This screens IR constructs. It says nothing about the linked runtime, dlopen
//! or FFI closure, so a zero-GC release still needs separate linked-image evidence.

use std::{env, fs, io::Read, process};

const MAX_IR_BYTES: u64 = 64 * 1024 * 1024;
const FORBIDDEN_INTRINSICS: &[&str] = &[
    "llvm.gcroot",
    "llvm.gcread",
    "llvm.gcwrite",
    "llvm.gc.statepoint",
    "llvm.gc.relocate",
    "llvm.gc.result",
    "llvm.experimental.gc.",
];
const FORBIDDEN_RUNTIME: &[&str] = &[
    "GC_malloc",
    "GC_realloc",
    "GC_gcollect",
    "GC_enable",
    "GC_disable",
    "mono_gc_",
    "g_gc_",
    "jl_gc_collect",
    "PyGC_Collect",
    "gc_collect",
    "gc_alloc",
    "gc_mark",
    "gc_sweep",
    "gc_root",
];

fn is_word(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_'
}

/// Return the end of the quoted run starting at `open`, past its closing quote.
/// LLVM escapes are `\HH` hex pairs, so a backslash never escapes a quote there,
/// but treating it as an escape matches what producers emit and is the stricter read.
fn quoted_end(line: &[u8], open: usize) -> Result<usize, String> {
    let mut i = open + 1;
    while i < line.len() {
        match line[i] {
            b'\\' => i += 2,
            b'"' => return Ok(i + 1),
            _ => i += 1,
        }
    }
    Err("unterminated LLVM IR string or quoted symbol".into())
}

fn check_symbol(name: &[u8]) -> Result<(), String> {
    let name = String::from_utf8_lossy(name);
    if FORBIDDEN_INTRINSICS.iter().any(|p| name.starts_with(p)) {
        return Err("LLVM GC intrinsic forbidden".into());
    }
    if FORBIDDEN_RUNTIME.iter().any(|p| name.starts_with(p)) {
        return Err("known tracing collector call forbidden".into());
    }
    Ok(())
}

fn audit_line(line: &[u8]) -> Result<(), String> {
    let mut i = 0;
    while i < line.len() {
        match line[i] {
            // Everything after a semicolon outside quotes is a comment.
            b';' => return Ok(()),
            b'@' if line.get(i + 1) == Some(&b'"') => {
                let end = quoted_end(line, i + 1)?;
                let name = &line[i + 2..end - 1];
                // Hex escapes could spell a forbidden name, and nothing here
                // decodes them, so refuse rather than guess.
                let escaped = name.windows(3).any(|w| {
                    w[0] == b'\\' && w[1].is_ascii_hexdigit() && w[2].is_ascii_hexdigit()
                });
                if escaped {
                    return Err("escaped quoted symbol requires semantic admission".into());
                }
                check_symbol(name)?;
                i = end;
            }
            b'@' => {
                let start = i + 1;
                let mut end = start;
                while end < line.len()
                    && (is_word(line[end]) || line[end] == b'.' || line[end] == b'$')
                {
                    end += 1;
                }
                check_symbol(&line[start..end])?;
                i = end.max(start);
            }
            // String constants and other quoted text are data, not IR constructs.
            b'"' => i = quoted_end(line, i)?,
            b'g' if line[i..].starts_with(b"gc")
                && (i == 0 || !is_word(line[i - 1]))
                && line.get(i + 2).is_some_and(u8::is_ascii_whitespace) =>
            {
                let next = line[i + 2..].iter().position(|b| !b.is_ascii_whitespace());
                if next.is_some_and(|skip| line[i + 2 + skip] == b'"') {
                    return Err("tracing GC strategy forbidden".into());
                }
                i += 2;
            }
            _ => i += 1,
        }
    }
    Ok(())
}

fn audit_ir(text: &str) -> Result<(), String> {
    for (index, line) in text.lines().enumerate() {
        audit_line(line.as_bytes()).map_err(|why| format!("line {}: {why}", index + 1))?;
    }
    Ok(())
}

fn audit_file(path: &str) -> Result<(), String> {
    // Open once and bind checks to the opened descriptor. Metadata followed by
    // fs::read(path) permits a path swap or file growth between the size check
    // and the allocation. Read at most MAX+1 bytes even if the file grows.
    let input = fs::File::open(path).map_err(|e| format!("{path}: {e}"))?;
    let metadata = input.metadata().map_err(|e| format!("{path}: {e}"))?;
    if !metadata.is_file() {
        return Err("IR input must be a regular file".into());
    }
    if metadata.len() > MAX_IR_BYTES {
        return Err("IR input exceeds 64 MiB gate limit".into());
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    input.take(MAX_IR_BYTES + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| format!("{path}: {e}"))?;
    if bytes.len() as u64 > MAX_IR_BYTES {
        return Err("IR input grew beyond 64 MiB gate limit while reading".into());
    }
    let text = String::from_utf8(bytes).map_err(|e| format!("{path}: not UTF-8: {e}"))?;
    audit_ir(&text)
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.len() != 1 {
        eprintln!("usage: audit_no_gc_ir program.ll");
        process::exit(64);
    }
    match audit_file(&args[0]) {
        Ok(()) => println!("STRICT_NO_GC_IR_SCREENED_ONLY (linked binary and FFI audit still required)"),
        Err(why) => {
            eprintln!("STRICT_NO_GC_IR_REJECTED: {why}");
            process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{audit_file, audit_ir, MAX_IR_BYTES};

    #[test]
    fn rejects_known_collector_ir() {
        for ir in [
            r#"define void @guest() gc "shadow-stack" { ret void }"#,
            r#"define void @"safe;label"() gc "shadow-stack" { ret void }"#,
            "define void @guest() gc\t\"statepoint-example\" { ret void }",
            "declare void @llvm.gcroot(ptr, ptr)",
            "declare token @llvm.gc.statepoint.p0(i64, i32, ptr, i32, i32, ...)",
            "declare void @llvm.experimental.gc.anything()",
            "declare ptr @GC_malloc(i64)",
            "declare ptr @GC_malloc_atomic(i64)",
            "declare void @gc_collect()",
            r#"declare void @"gc_collect"()"#,
            r#"declare void @"llvm.g\63root"(ptr, ptr)"#,
            r#"@x = constant [2 x i8] c"unterminated"#,
        ] {
            assert!(audit_ir(ir).is_err(), "admitted: {ir}");
        }
    }

    #[test]
    fn file_reader_rejects_nonregular_and_oversized_inputs() {
        use std::fs::{self, OpenOptions};
        use std::time::{SystemTime, UNIX_EPOCH};

        let nonce = SystemTime::now().duration_since(UNIX_EPOCH)
            .expect("clock").as_nanos();
        let path = std::env::temp_dir()
            .join(format!("atto-no-gc-audit-{}-{nonce}.ll", std::process::id()));
        let created = OpenOptions::new().write(true).create_new(true)
            .open(&path).expect("create isolated test file");
        drop(created);
        let name = path.to_str().expect("temp path UTF-8");
        let result = audit_file(name);
        assert!(result.is_ok(), "empty regular LLVM IR is valid: {result:?}");
        let oversized = OpenOptions::new().write(true).open(&path).expect("open");
        oversized.set_len(MAX_IR_BYTES + 1).expect("make sparse oversized file");
        drop(oversized);
        let result = audit_file(name);
        assert!(result.unwrap_err().contains("64 MiB"));
        fs::remove_file(path).expect("remove test file");

        let dir = std::env::temp_dir();
        let result = audit_file(dir.to_str().expect("temp dir UTF-8"));
        assert!(result.is_err(), "directories must not be accepted as IR input");
    }

    #[test]
    fn admits_ir_without_collector_constructs() {
        for ir in [
            "define i64 @main() { ret i64 42 }",
            "; define void @guest() gc \"shadow-stack\" { }",
            "declare void @my_gc_collect()",
            "declare void @dogc()",
            // Guest strings are data: these must not trip a name or strategy match.
            r#"@msg = constant [10 x i8] c"@gc_alloc\00""#,
            r#"@msg = constant [10 x i8] c"gc \22x\22\00""#,
            r#"source_filename = "gc_collect.atto""#,
            r#"define void @"safe;label"() { ret void }"#,
        ] {
            assert!(audit_ir(ir).is_ok(), "rejected: {ir}");
        }
    }
}
