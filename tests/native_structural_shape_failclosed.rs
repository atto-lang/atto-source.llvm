//! Real oreslang-llvmc structural-width contract.
//! No mocks, no private-source mirroring, no Python, and no runtime reflection.
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
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH)
            .expect("clock").as_nanos();
        let dir = std::env::temp_dir().join(format!(
            "oreslang-structural-{}-{}", std::process::id(), nanos));
        fs::create_dir(&dir).expect("create temporary test directory");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&dir, fs::Permissions::from_mode(0o700))
                .expect("secure test directory");
        }
        Self(dir)
    }
}

fn compile(compiler: &Path, tmp: &Path, input: &str) -> std::process::Output {
    let source = tmp.join("probe.ores");
    fs::write(&source, input).expect("write fixture");
    let result = Command::new(compiler).arg(&source)
        .current_dir(tmp).output().expect("execute real native compiler");
    assert_eq!(fs::read_to_string(&source).expect("read fixture"), input,
        "compiler modified source");
    let unexpected: Vec<_> = read_dir(tmp).expect("list artifacts")
        .map(|e| e.expect("directory entry").file_name())
        .filter(|name| name != "probe.ores").collect();
    assert!(unexpected.is_empty(),
        "compiler published artifacts during admission: {:?}", unexpected);
    result
}

fn structural_positive() -> &'static str {
    r#"define interface PointLike as
  x: int;
  y: int;
end

define class Point3d as
  pub val int x: 1;
  pub val int y: 2;
  pub val int z: 3;
end

fnc sum(structural PointLike point): int {
  return point.x + point.y;
}

pub fnc main(): int {
  let Point3d point = new Point3d();
  return sum(point);
}
"#
}

fn run_native(clang: &Path, tmp: &Path, ir: &[u8]) {
    let ll = tmp.join("structural.ll");
    let exe = tmp.join("structural-native");
    fs::write(&ll, ir).expect("write structural LLVM IR");
    let build = Command::new(clang)
        .arg(&ll).arg("-o").arg(&exe)
        .current_dir(tmp).output().expect("execute clang");
    assert!(build.status.success(),
        "clang rejected structural LLVM IR: status={:?}, stderr={}",
        build.status, String::from_utf8_lossy(&build.stderr));
    let run = Command::new(&exe).current_dir(tmp)
        .output().expect("execute structural native binary");
    assert_eq!(run.status.code(), Some(3),
        "width-subtyped structural native program must return 3: stderr={}",
        String::from_utf8_lossy(&run.stderr));
    fs::remove_file(&ll).expect("remove structural LLVM IR");
    fs::remove_file(&exe).expect("remove structural native binary");
}

fn main() {
    let mut args = std::env::args_os().skip(1);
    let compiler = args.next()
        .expect("usage: native_structural_shape_failclosed COMPILER_PATH [CLANG_PATH]");
    let compiler = PathBuf::from(compiler).canonicalize()
        .expect("compiler must exist");
    let clang = args.next().map(PathBuf::from);

    let tmp = TestDir::create();
    let scalar = compile(&compiler, &tmp.0,
        "pub fnc main(): int { return 42; }\n");
    assert!(scalar.status.success() && scalar.stdout.starts_with(b"; ModuleID"),
        "real scalar control must emit LLVM ModuleID: status={:?}, stderr={}",
        scalar.status, String::from_utf8_lossy(&scalar.stderr));

    let structural = compile(&compiler, &tmp.0, structural_positive());
    assert!(structural.status.success() &&
            structural.stdout.starts_with(b"; ModuleID"),
        "structural width subtype must emit LLVM IR: status={:?}, stderr={}",
        structural.status, String::from_utf8_lossy(&structural.stderr));
    let ir = String::from_utf8_lossy(&structural.stdout);
    assert!(ir.contains("structural.value"),
        "structural lowering must materialize explicit scalar projection loads");
    assert!(!ir.contains("structural.reflect") && !ir.contains("structural.cast"),
        "structural lowering must not use reflection or raw layout casts");

    if let Some(clang) = clang {
        let clang = clang.canonicalize().expect("clang must exist");
        run_native(&clang, &tmp.0, &structural.stdout);
    }

    let probes = [
        ("malformed legacy interface parameter",
            "define interface HasId\n id:int;\nend\nfnc take(structural HasId v):int{return v.id;}\n"),
        ("inline record parameter",
            "fnc take(structural { id:int } v):int{return v.id;}\n"),
        ("missing public field",
r#"define interface PointLike as
  x: int;
  y: int;
end
define class Point as
  pub val int x: 1;
end
fnc sum(structural PointLike point): int { return point.x + point.y; }
pub fnc main(): int { let Point point = new Point(); return sum(point); }
"#),
        ("private field cannot satisfy shape",
r#"define interface PointLike as
  x: int;
  y: int;
end
define class Point as
  pub val int x: 1;
  val int y: 2;
end
fnc sum(structural PointLike point): int { return point.x + point.y; }
pub fnc main(): int { let Point point = new Point(); return sum(point); }
"#),
        ("field type mismatch",
r#"define interface PointLike as
  x: int;
  y: int;
end
define class Point as
  pub val int x: 1;
  pub val bool y: true;
end
fnc sum(structural PointLike point): int { return point.x + point.y; }
pub fnc main(): int { let Point point = new Point(); return sum(point); }
"#),
        ("mutation through readonly structural view",
r#"define interface PointLike as
  x: int;
end
define class Point as
  pub let int x = 1;
end
fnc mutate(structural PointLike point): int {
  point.x = 2;
  return point.x;
}
pub fnc main(): int { let Point point = new Point(); return mutate(point); }
"#),
        ("explicit mutable structural borrow",
r#"define interface PointLike as
  x: int;
end
fnc mutate(borrow mut structural PointLike point): int { return point.x; }
"#),
        ("named shape pattern",
            "fnc main():int { match 1 over on structural HasId v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("inline shape pattern",
            "fnc main():int { match 1 over on structural { id:int } v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("tuple parameter",
            "fnc take(structural Tuple<int,string> v):int{return v[0];}\n"),
        ("tuple pattern",
            "fnc main():int { match 1 over on structural (int,int) v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("actor parameter",
            "actor fnc take(structural { id:int } v):int{return 1;}\n"),
    ];
    for (name, src) in probes {
        let output = compile(&compiler, &tmp.0, src);
        assert!(!output.status.success(),
            "{name}: unsupported/invalid structural program compiled");
        assert!(output.stdout.iter().all(u8::is_ascii_whitespace),
            "{name}: emitted LLVM IR on rejection");
        assert!(!output.stderr.is_empty(), "{name}: missing diagnostic");
    }

    println!(
        "PASS: scalar control; structural width subtype -> IR{}; {} negative no-IR/no-artifact checks",
        if clang.is_some() { " -> native return 3" } else { "" },
        probes.len());
}
