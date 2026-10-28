//! Actual oreslang-llvmc admission test: native structural shapes fail closed.
//! No mocks, no private-source mirroring, and no Python. An actual scalar
//! source must compile to LLVM IR before a negative admission can pass.
use std::fs::{self, read_dir};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

struct TestDir(PathBuf);
impl Drop for TestDir {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}
impl TestDir {
    fn create() -> Self {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).expect("clock").as_nanos();
        let dir = std::env::temp_dir().join(format!("oreslang-structural-{}-{}", std::process::id(), nanos));
        fs::create_dir(&dir).expect("create temporary test directory");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&dir, fs::Permissions::from_mode(0o700)).expect("secure test directory");
        }
        Self(dir)
    }
}
fn compile(compiler: &Path, tmp: &Path, input: &str) -> std::process::Output {
    let source = tmp.join("probe.ores");
    fs::write(&source, input).expect("write fixture");
    let result = Command::new(compiler).arg(&source)
        .current_dir(tmp).output().expect("execute real native compiler");
    assert_eq!(fs::read_to_string(&source).expect("read fixture"), input, "compiler modified source");
    let unexpected:Vec<_> = read_dir(tmp).expect("list artifacts")
        .map(|e| e.expect("directory entry").file_name())
        .filter(|name| name != "probe.ores").collect();
    assert!(unexpected.is_empty(), "compiler published artifacts during admission: {:?}", unexpected);
    result
}
fn main() {
    let compiler = std::env::args_os().nth(1).expect("usage: native_structural_shape_failclosed COMPILER_PATH");
    let compiler = PathBuf::from(compiler).canonicalize().expect("compiler must exist");
    let tmp = TestDir::create();
    let positive = compile(&compiler, &tmp.0, "pub fnc main(): int { return 42; }\n");
    assert!(positive.status.success() && positive.stdout.starts_with(b"; ModuleID"),
        "real scalar control must emit LLVM ModuleID: status={:?}, stderr={}",
        positive.status, String::from_utf8_lossy(&positive.stderr));
    // Real compiler positive control for bounded scalar value pipelines.
    // The |> operator must lower to nested typed calls (not lexical erasure).
    let pipe_source = "pub fnc main(): int { return 3 |> twice |> plus(1); }\nfnc twice(int n): int { return n * 2; }\nfnc plus(int n, int k): int { return n + k; }\n";
    let piped = compile(&compiler, &tmp.0, pipe_source);
    assert!(piped.status.success() && piped.stdout.starts_with(b"; ModuleID"),
        "real scalar pipeline must lower: {}", String::from_utf8_lossy(&piped.stderr));
    for symbol in [b"@twice(".as_slice(), b"@plus(".as_slice()] {
        assert!(piped.stdout.windows(symbol.len()).any(|x| x == symbol),
            "scalar pipeline must retain checked stage calls: {:?}", symbol);
    }
    let probes = [
        ("interface parameter", "define interface HasId\n id:int;\nend\nfnc take(structural HasId v):int{return v.id;}\n"),
        ("inline record parameter", "fnc take(structural { id:int } v):int{return v.id;}\n"),
        ("named shape pattern", "fnc main():int { match 1 over on structural HasId v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("inline shape pattern", "fnc main():int { match 1 over on structural { id:int } v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("tuple parameter", "fnc take(structural Tuple<int,string> v):int{return v[0];}\n"),
        ("tuple pattern", "fnc main():int { match 1 over on structural (int,int) v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("actor parameter", "actor fnc take(structural { id:int } v):int{return 1;}\n")
    ];
    for (name, src) in probes {
        let output = compile(&compiler, &tmp.0, src);
        assert!(!output.status.success(), "{name}: unsupported structural program compiled");
        assert!(output.stdout.iter().all(u8::is_ascii_whitespace), "{name}: emitted LLVM IR on rejection");
        assert!(!output.stderr.is_empty(), "{name}: missing diagnostic");
    }
    // Distinguish a compiler-owned unsupported-structural diagnostic from
    // accidental generic parser failure. These exact constructs must reject
    // after recognizing their intended syntactic position.
    let specific = [
        ("callable", "structural fnc main(): int { return 1; }\n", "native structural callable requires"),
        ("routine callable", "structural routine main(): int { return 1; }\n", "native structural callable requires"),
        ("parameter", "fnc check(structural Shape s): int { return 1; }\n", "native structural parameter requires"),
        ("routine parameter", "routine check(structural Shape s): int { return 1; }\n", "native structural parameter requires"),
        ("procedural match", "pub fnc main(): int { do match 1 over on structural { id:int } v -> { return 1; } on _ -> { return 0; } end return 1; }\n", "native structural match expression requires"),
        ("value match", "pub fnc main(): int { return match 1 over on structural { id:int } v -> { return 1; } on _ -> { return 0; } end; }\n", "native structural match expression requires"),
    ];
    for (name, src, expected) in specific {
        let output = compile(&compiler, &tmp.0, src);
        assert_eq!(output.status.code(), Some(1), "{name}: expected compiler-owned rejection");
        assert!(output.stdout.is_empty(), "{name}: emitted LLVM IR on rejection");
        assert!(String::from_utf8_lossy(&output.stderr).contains(expected),
                "{name}: wrong or generic diagnostic: {}", String::from_utf8_lossy(&output.stderr));
    }
    // Inverted-class lowering is unavailable in the scalar native subset.
    // Generic parser or unrelated type errors cannot be counted as admission.
    let inverted = [
        ("root", "define inverted class Particle as x: int; end\n"),
        ("define-pub", "define pub inverted class Particle as x: int; end\n"),
        ("pub-define", "pub define inverted class Particle as x: int; end\n"),
        ("module", "define module Physics as\n define inverted class Particle as x: int; end\n pub fnc main(): int { return 0; }\nend\n"),
    ];
    for (name, src) in inverted {
        let output = compile(&compiler, &tmp.0, src);
        assert_eq!(output.status.code(), Some(1), "{name}: required inverted declaration rejection");
        assert!(output.stdout.is_empty(), "{name}: emitted IR on inverted declaration");
        assert!(String::from_utf8_lossy(&output.stderr).contains("INVERTED_BACKEND_UNSUPPORTED"),
            "{name}: expected compiler-owned inverted-class diagnostic: {}", String::from_utf8_lossy(&output.stderr));
    }
    println!("PASS: 1 scalar positive, {} fail-closed shapes, {} structural diagnostics, {} inverted boundaries",
        probes.len(), specific.len(), inverted.len());
}
