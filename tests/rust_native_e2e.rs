// Dependency-free Rust driver: actual Atto source -> LLVM IR -> Clang -> native binary.
use std::{env, fs, path::{Path, PathBuf}, process::{Command, Output, Stdio}, time::{SystemTime, UNIX_EPOCH}};

#[path = "rust_native_e2e/guard_positive.rs"]
mod guard_positive;
#[path = "rust_native_e2e/guard_negative.rs"]
mod guard_negative;
#[path = "rust_native_e2e/pointer_positive.rs"]
mod pointer_positive;
#[path = "rust_native_e2e/pointer_negative.rs"]
mod pointer_negative;
#[path = "rust_native_e2e/static_ffi.rs"]
mod static_ffi;
#[path = "rust_native_e2e/ffi_normal_positive.rs"]
mod ffi_normal_positive;
#[path = "rust_native_e2e/ffi_normal_negative.rs"]
mod ffi_normal_negative;
#[path = "rust_native_e2e/functional_pipelines.rs"]
mod functional_pipelines;

pub struct Harness {
    compiler: PathBuf,
    clang: PathBuf,
    root: PathBuf,
    count: usize,
}

fn output(mut command: Command, label: &str) -> Output {
    command.stdin(Stdio::null()).output().unwrap_or_else(|e| panic!("{} could not execute: {}", label, e))
}

fn must_succeed(result: &Output, label: &str) {
    assert!(result.status.success(),
        "{} failed exit {:?}; stdout={}; stderr={}", label, result.status.code(),
        String::from_utf8_lossy(&result.stdout), String::from_utf8_lossy(&result.stderr));
}

impl Harness {
    fn new(compiler: PathBuf, clang: PathBuf) -> Self {
        assert!(compiler.is_file(), "REAL_LLVM_COMPILER_REQUIRED: {:?}", compiler);
        assert!(clang.is_file(), "REAL_CLANG_REQUIRED: {:?}", clang);
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let root = env::temp_dir().join(format!("atto-rust-native-{}-{}", std::process::id(), stamp));
        fs::create_dir(&root).expect("create isolated native guest root");
        Self { compiler, clang, root, count: 0 }
    }

    fn compile(&mut self, name: &str, source: &str, flags: &[&str]) -> (PathBuf, Output) {
        assert!(name.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-'));
        assert!(source.len() < 4096, "fixture budget exceeded");
        self.count += 1;
        let case = self.root.join(format!("{:03}_{}", self.count, name));
        fs::create_dir(&case).expect("create fresh case");
        let source_path = case.join("input.atto");
        fs::write(&source_path, source).expect("write source");
        let before = fs::read(&source_path).expect("read original source");
        let mut command = Command::new(&self.compiler);
        command.current_dir(&case).arg("--target=host").args(flags).arg(&source_path);
        let result = output(command, name);
        assert_eq!(before, fs::read(&source_path).expect("read checked source"), "compiler modified its input");
        (case, result)
    }

    pub fn positive(
        &mut self, name: &str, source: &str, flags: &[&str],
        expected_exit: i32, sanitizer: bool, approved_c_shim: Option<&str>,
    ) {
        let (case, compile) = self.compile(name, source, flags);
        must_succeed(&compile, &format!("Atto LLVM source compiler {}", name));
        let ir = String::from_utf8_lossy(&compile.stdout);
        assert!(ir.contains("define "), "{}: compiler returned no LLVM function", name);
        if approved_c_shim.is_some() {
            assert!(ir.contains("declare i64 @atto_add_i64(i64, i64)"), "{}: wrong external C ABI", name);
            assert!(ir.contains("call i64 @atto_add_i64"), "{}: missing LLVM external call", name);
        }
        let ir_path = case.join("guest.ll");
        fs::write(&ir_path, &compile.stdout).expect("write emitted LLVM IR");
        let shim_obj = approved_c_shim.map(|text| {
            let path = case.join("approved.c");
            let obj = case.join("approved.o");
            fs::write(&path, text).expect("write fixed approved C symbol");
            let mut cc = Command::new(&self.clang);
            cc.arg("-std=c11").arg("-c").arg(&path).arg("-o").arg(&obj);
            must_succeed(&output(cc, "compile fixed C ABI shim"), "compile fixed C ABI shim");
            obj
        });
        self.compile_and_execute_native(&case, &ir_path, shim_obj.as_deref(), expected_exit, false, name);
        if sanitizer {
            self.compile_and_execute_native(&case, &ir_path, shim_obj.as_deref(), expected_exit, true, name);
        }
        println!("ATTO_RUST_NATIVE_POSITIVE_PASS={} expected_exit={} sanitizer={}", name, expected_exit, sanitizer);
    }

    fn compile_and_execute_native(
        &self, case: &Path, ir: &Path, shim: Option<&Path>,
        expected: i32, sanitizer: bool, name: &str,
    ) {
        let executable = case.join(if sanitizer { "guest.sanitized.native" } else { "guest.native" });
        let mut cc = Command::new(&self.clang);
        if sanitizer {
            cc.args(["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]);
        } else {
            cc.arg("-O2");
        }
        cc.arg("-Wno-override-module").args(["-x", "ir"]).arg(ir);
        if let Some(shim_obj) = shim {
            cc.args(["-x", "none"]).arg(shim_obj);
        }
        cc.arg("-o").arg(&executable);
        must_succeed(&output(cc, &format!("Clang link {}", name)), &format!("Clang link {}", name));
        assert!(fs::metadata(&executable).expect("native artifact missing").len() > 0, "native executable empty");
        let mut command = Command::new(&executable);
        command.current_dir(case);
        if sanitizer {
            command.env("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1");
            command.env("UBSAN_OPTIONS", "halt_on_error=1");
        }
        let executed = output(command, &format!("native guest {}", name));
        assert_eq!(executed.status.code(), Some(expected),
            "{} native exit mismatch sanitizer={}; stdout={}; stderr={}", name, sanitizer,
            String::from_utf8_lossy(&executed.stdout), String::from_utf8_lossy(&executed.stderr));
    }

    /// Observe the complete signed i64 guest return through a fixed test-only
    /// LLVM entry adapter. The emitted @atto_func_N body is unchanged: only
    /// the compiler-generated i32 main wrapper is renamed to avoid OS-exit
    /// truncation. The observer calls that same native guest body and prints
    /// its 64-bit result using the OS C printf ABI.
    pub fn positive_full_i64(&mut self, name: &str, source: &str, expected: &str) {
        let (case, compiled) = self.compile(name, source, &[]);
        must_succeed(&compiled, &format!("LLVM i64 guest admission {name}"));
        let original = String::from_utf8(compiled.stdout)
            .expect("compiler emitted valid UTF-8 LLVM IR");
        let entry = "define i32 @main()";
        assert_eq!(original.matches(entry).count(), 1,
            "{name}: requires exactly one compiled main entry");
        let marker = "%answer = call i64 @atto_func_";
        let at = original.find(marker)
            .expect("compiler main must call native Atto i64 function") + marker.len();
        let digits: String = original[at..].chars()
            .take_while(|c| c.is_ascii_digit()).collect();
        assert!(!digits.is_empty(), "{name}: missing native guest function index");
        let idx: usize = digits.parse().expect("valid typed function index");
        assert!(original.contains(&format!("define internal i64 @atto_func_{idx}(")),
            "{name}: full-width Atto guest symbol not found");
        let mut observed_ir = original.replacen(
            entry, "define i32 @atto_unobserved_process_entry()", 1);
        observed_ir.push_str(&format!(r#"
@atto_observer_fmt = private constant [6 x i8] c"%lld\0A\00"
declare i32 @printf(ptr, ...)
define i32 @main() {{
entry:
  %atto_observed_value = call i64 @atto_func_{idx}()
  %atto_written = call i32 (ptr, ...) @printf(ptr getelementptr inbounds ([6 x i8], ptr @atto_observer_fmt, i64 0, i64 0), i64 %atto_observed_value)
  ret i32 0
}}
"#));
        let ir = case.join("full-i64-observer.ll");
        fs::write(&ir, observed_ir).expect("write fixed observer LLVM IR");
        let executable = case.join("observed-i64.native");
        let mut cc = Command::new(&self.clang);
        cc.args(["-O2", "-Wno-override-module", "-x", "ir"])
            .arg(&ir).arg("-o").arg(&executable);
        must_succeed(&output(cc, "Clang full-i64 observer build"),
            "Clang full-i64 observer build");
        let executed = output(Command::new(&executable), "compiled native i64 guest observer");
        assert_eq!(executed.status.code(), Some(0),
            "{name}: observer process failed");
        assert_eq!(executed.stdout, format!("{expected}\n").as_bytes(),
            "{name}: full-width native guest value was truncated or miscompiled");
        assert!(executed.stderr.is_empty(), "{name}: unexpected observer errors");
        println!("ATTO_LLVM_FULL_I64_NATIVE_GUEST_PASS={name} value={expected}");
    }

    /// Checked overflow is a *runtime* native failure after successful
    /// compilation, never a compile-time literal-range rejection or wrap.
    pub fn runtime_checked_overflow(&mut self, name: &str, source: &str) {
        let (case, compiled) = self.compile(name, source, &[]);
        must_succeed(&compiled, &format!("LLVM checked overflow guest admission {name}"));
        let ir_string = String::from_utf8_lossy(&compiled.stdout);
        assert!(ir_string.contains("llvm.sadd.with.overflow.i64"),
            "{name}: emitted IR lacks checked signed add");
        assert!(ir_string.contains("llvm.trap"),
            "{name}: emitted IR has no checked-overflow trap");
        let ir = case.join("overflow.ll");
        fs::write(&ir, &compiled.stdout).expect("write actual overflow LLVM IR");
        let executable = case.join("overflow.native");
        let mut cc = Command::new(&self.clang);
        cc.args(["-O2", "-Wno-override-module", "-x", "ir"])
            .arg(&ir).arg("-o").arg(&executable);
        must_succeed(&output(cc, "Clang checked-overflow guest"),
            "Clang checked-overflow guest");
        let result = output(Command::new(&executable), "execute checked-overflow guest");
        assert!(!result.status.success(),
            "{name}: overflowing native guest returned normally");
        assert!(result.stdout.is_empty(),
            "{name}: overflowing guest emitted a normal result");
        println!("ATTO_LLVM_NATIVE_CHECKED_OVERFLOW_TRAP_PASS={name}");
    }

    pub fn negative(&mut self, name: &str, source: &str, flags: &[&str], diagnostic: Option<&str>) {
        let (case, result) = self.compile(name, source, flags);
        assert!(!result.status.success(), "unsupported Atto source was admitted: {}", name);
        assert!(result.stdout.is_empty(),
            "rejected source emitted partial LLVM IR/stdout for {}: {}", name, String::from_utf8_lossy(&result.stdout));
        if let Some(wanted) = diagnostic {
            let err = String::from_utf8_lossy(&result.stderr).to_ascii_lowercase();
            assert!(err.contains(&wanted.to_ascii_lowercase()), "{} missing diagnostic {:?}: {}", name, wanted, err);
        }
        let entries: Vec<_> = fs::read_dir(&case).expect("inspect rejected source directory")
            .map(|e| e.expect("list entry").file_name().to_string_lossy().into_owned()).collect();
        assert_eq!(entries, vec!["input.atto"], "{} left compiler artifacts: {:?}", name, entries);
        println!("ATTO_RUST_NATIVE_NO_IR_PASS={}", name);
    }

    fn finish(self) {
        println!("ATTO_RUST_NATIVE_ACTUAL_GUEST_CASES_PASS={}", self.count);
        fs::remove_dir_all(self.root).expect("remove completed guest workspace");
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    assert_eq!(args.len(), 3, "usage: rust_native_e2e COMPILER CLANG");
    let mut h = Harness::new(PathBuf::from(&args[1]), PathBuf::from(&args[2]));
    guard_positive::run(&mut h);
    guard_negative::run(&mut h);
    pointer_positive::run(&mut h);
    pointer_negative::run(&mut h);
    static_ffi::run(&mut h);
    ffi_normal_positive::run(&mut h);
    ffi_normal_negative::run(&mut h);
    functional_pipelines::run(&mut h);
    h.finish();
}
