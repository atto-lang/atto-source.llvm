// Compile a parameterized AttoLang guest to LLVM IR, bind its true i64 ABI,
// and compare full-width native results against an independent checked Rust oracle.
// No Python, hand-authored LLVM IR, host substitute, or low-byte exit oracle.
// Evidence tier: NONE until this binary executes on an exact-head source checkout.
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::time::{SystemTime, UNIX_EPOCH};

const FIXTURE: &str = "tests/fixtures/hpc-reduction-v1/i64-parameterized-tiles.atto";
const N_RANDOM: usize = 96;
const VECTOR_MATRIX_SHA256: &str = "b921d3a36a08917bd5ce7681eb9e00c7b37bb62e8308e0b8615c0c7ffb3ff575";
const OBSERVER_C: &str = r#"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
_Static_assert(sizeof(int64_t) == sizeof(long long), "observer ABI requires i64");
extern int64_t tile_reduce8(int64_t a, int64_t b, int64_t c, int64_t d,
                            int64_t e, int64_t f, int64_t g, int64_t h);
int main(int argc, char **argv) {
    if (argc != 9) return 64;
    int64_t values[8];
    for (int i = 0; i < 8; ++i) {
        char *end = NULL;
        errno = 0;
        long long parsed = strtoll(argv[i + 1], &end, 10);
        if (errno != 0 || end == argv[i + 1] || *end != '\0') return 65;
        values[i] = (int64_t)parsed;
    }
    printf("%" PRId64 "\n",
           tile_reduce8(values[0], values[1], values[2], values[3],
                        values[4], values[5], values[6], values[7]));
    return 0;
}
"#;

fn fail(message: impl AsRef<str>) -> ! {
    eprintln!("ATTO_HPC_I64_DIFF_REJECTED: {}", message.as_ref());
    std::process::exit(1)
}

fn require(condition: bool, message: impl AsRef<str>) {
    if !condition {
        fail(message);
    }
}

fn output(program: &str, args: &[&str]) -> Output {
    Command::new("timeout")
        .args(["-k", "5s", "60s", program])
        .args(args)
        .output()
        .unwrap_or_else(|err| fail(format!("cannot run {program}: {err}")))
}

fn successful(program: &str, args: &[&str]) -> String {
    let result = output(program, args);
    require(
        result.status.success(),
        format!(
            "{program} exit {:?}: {}",
            result.status.code(),
            String::from_utf8_lossy(&result.stderr)
        ),
    );
    String::from_utf8(result.stdout)
        .unwrap_or_else(|err| fail(format!("{program} returned invalid UTF-8: {err}")))
        .trim()
        .to_owned()
}

fn successful_at(stage: &str, program: &str, args: &[&str]) -> String {
    let result = output(program, args);
    require(
        result.status.success(),
        format!(
            "stage={stage} program={program} exit={:?} stderr={}",
            result.status.code(),
            String::from_utf8_lossy(&result.stderr)
        ),
    );
    String::from_utf8(result.stdout)
        .unwrap_or_else(|err| fail(format!("stage={stage} invalid UTF-8: {err}")))
        .trim()
        .to_owned()
}

fn sha256(path: &Path) -> String {
    let name = path.to_str().unwrap_or_else(|| fail("non-UTF8 file path"));
    let digest = successful("sha256sum", &[name]);
    let hex = digest.split_whitespace().next().unwrap_or("");
    require(
        hex.len() == 64 && hex.bytes().all(|c| c.is_ascii_hexdigit()),
        "sha256sum returned malformed digest",
    );
    hex.to_owned()
}

fn checked_file(path: &Path) {
    let meta = fs::symlink_metadata(path)
        .unwrap_or_else(|err| fail(format!("cannot stat {}: {err}", path.display())));
    require(
        meta.file_type().is_file() && !meta.file_type().is_symlink(),
        format!("not a regular file: {}", path.display()),
    );
}

fn require_head_fixture() -> (String, String) {
    let head = env::var("EXPECTED_HEAD").unwrap_or_default();
    require(
        head.len() == 40 && head.bytes().all(|c| c.is_ascii_hexdigit()),
        "EXPECTED_HEAD must be a lowercase 40-hex commit",
    );
    require(
        successful("git", &["rev-parse", "HEAD"]) == head,
        "checkout does not match EXPECTED_HEAD",
    );
    let tree = successful("git", &["rev-parse", "HEAD^{tree}"]);
    require(
        tree.len() == 40 && tree.bytes().all(|c| c.is_ascii_hexdigit()),
        "invalid source tree",
    );
    checked_file(Path::new(FIXTURE));
    let committed = successful("git", &["rev-parse", &format!("HEAD:{FIXTURE}")]);
    let working = successful("git", &["hash-object", "--no-filters", "--", FIXTURE]);
    require(committed == working, "guest source differs from committed blob");
    require(
        successful("git", &["status", "--porcelain", "--", FIXTURE]).is_empty(),
        "guest source is dirty",
    );
    (head, tree)
}

struct Scratch(PathBuf);

impl Scratch {
    fn new() -> Self {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_else(|err| fail(format!("clock unavailable: {err}")))
            .as_nanos();
        let dir = env::temp_dir().join(format!(
            "atto-hpc-i64-diff-{}-{nanos}",
            std::process::id()
        ));
        fs::create_dir(&dir).unwrap_or_else(|err| fail(format!("mkdir: {err}")));
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&dir, fs::Permissions::from_mode(0o700))
                .unwrap_or_else(|err| fail(format!("chmod: {err}")));
        }
        Self(dir)
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

// The Atto source's tile4 is ((a+b)+c)+d. The second tile follows
// the same grouping, and tile0 + tile1 is the only merge operation.
fn oracle(values: [i64; 8]) -> Option<i64> {
    let one = values[..4]
        .iter()
        .copied()
        .try_fold(0_i64, |total, item| total.checked_add(item))?;
    let two = values[4..]
        .iter()
        .copied()
        .try_fold(0_i64, |total, item| total.checked_add(item))?;
    one.checked_add(two)
}

fn vectors() -> Vec<[i64; 8]> {
    let mut cases = vec![
        [0; 8],
        [1, 2, 3, 4, 5, 6, 7, 8],
        [-1, -2, -3, -4, -5, -6, -7, -8],
        [i64::MAX, -i64::MAX, 10, -5, i64::MIN, i64::MAX, 2, 3],
        [i64::MIN, i64::MAX, i64::MAX, -i64::MAX, 7, -7, 0, 0],
        [i64::MAX, 0, 0, 0, -i64::MAX, 0, 0, 0],
        [i64::MIN, 1, 0, 0, i64::MAX, -1, 0, 0],
        [10_000_000_000_000_000, 1, -10_000_000_000_000_000, 1,
         -10_000_000_000_000_000, 1, 10_000_000_000_000_000, 1],
        [5_000_000_000, 4_000_000_000, -3_000_000_000, 7, -9, 10, 0, 0],
        [255, 1, 1, 1, 1, 1, 1, 1],
        [i64::MAX, -1, 0, 0, -i64::MAX, 1, 0, 0],
    ];
    let mut state = 0x5EED_F00D_BADC_0FFE_u64;
    for _ in 0..N_RANDOM {
        let mut values = [0_i64; 8];
        for item in &mut values {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            *item = (state % 1_000_001) as i64 - 500_000;
        }
        cases.push(values);
    }
    cases
}

fn rewrite_os_entrypoint(ir: &str) -> String {
    let target = "define i32 @main()";
    require(
        ir.matches(target).count() == 1,
        "compiler IR must contain exactly one OS main entrypoint",
    );
    require(
        ir.contains("define i64 @__oreslang_guest_main("),
        "compiler IR lacks actual Atto guest main",
    );
    require(
        ir.contains("define i64 @tile_reduce8("),
        "compiler IR lacks external parameterized i64 tile function",
    );
    ir.replacen(target, "define i32 @__atto_guest_os_entry_unused()", 1)
}

#[cfg(test)]
mod unit_tests {
    use super::{oracle, rewrite_os_entrypoint, vectors, N_RANDOM};

    #[test]
    fn checked_oracle_bounds_and_cancellation() {
        assert_eq!(oracle([1, 2, 3, 4, 5, 6, 7, 8]), Some(36));
        assert_eq!(
            oracle([i64::MAX, -i64::MAX, 10, -5, i64::MIN, i64::MAX, 2, 3]),
            Some(9)
        );
        assert_eq!(
            oracle([i64::MAX, 1, 0, 0, 0, 0, 0, 0]),
            None,
            "overflow must never be accepted as a valid golden result"
        );
    }

    #[test]
    fn fixed_seed_case_matrix_is_bounded() {
        let samples = vectors();
        assert_eq!(samples.len(), N_RANDOM + 11);
        assert!(samples.iter().all(|row| oracle(*row).is_some()));
        assert!(samples.iter().any(|row| oracle(*row).unwrap() < 0));
        assert!(samples.iter().any(|row| oracle(*row).unwrap() > 255));
    }

    #[test]
    fn observer_replacement_requires_real_guest_functions() {
        let valid = "define i64 @__oreslang_guest_main() {\nret i64 42\n}\n\
                     define i64 @tile_reduce8(i64 %a) {\nret i64 %a\n}\n\
                     define i32 @main() {\nret i32 42\n}\n";
        let result = rewrite_os_entrypoint(valid);
        assert!(result.contains("define i32 @__atto_guest_os_entry_unused()"));
        assert!(!result.contains("define i32 @main()"));
    }
}

fn main() {
    let (head, tree) = require_head_fixture();
    let compiler = Path::new("build/oreslang-llvmc");
    let auditor = Path::new("build/oreslang-no-gc-ir-audit");
    checked_file(compiler);
    checked_file(auditor);
    let compiler_hash = sha256(compiler);
    let source_hash = sha256(Path::new(FIXTURE));
    let scratch = Scratch::new();
    let original_ir = successful_at(
        "guest_compiler_ir", "build/oreslang-llvmc",
        &["--target=host", FIXTURE],
    );
    let patched = rewrite_os_entrypoint(&original_ir);
    let ir_path = scratch.0.join("guest-observed.ll");
    let c_path = scratch.0.join("observer.c");
    fs::write(&ir_path, patched).unwrap_or_else(|err| fail(format!("write IR: {err}")));
    fs::write(&c_path, OBSERVER_C).unwrap_or_else(|err| fail(format!("write observer: {err}")));
    let ir_str = ir_path.to_str().unwrap_or_else(|| fail("invalid IR path"));
    let c_str = c_path.to_str().unwrap_or_else(|| fail("invalid C path"));
    successful_at("native_no_gc_ir_audit", "build/oreslang-no-gc-ir-audit", &[ir_str]);
    let ir_hash = sha256(&ir_path);
    let test_vectors = vectors();
    require(
        test_vectors.len() == N_RANDOM + 11,
        "unexpected differential vector count",
    );
    // Canonical decimal-comma-LF matrix digest is independently pinned
    // by Java/Graal and platform-conformance. This detects accidental
    // drift in vector order, signed representation or PRNG semantics.
    let canonical_rows = test_vectors
        .iter()
        .map(|row| {
            row.iter()
                .map(i64::to_string)
                .collect::<Vec<_>>()
                .join(",")
                + "\n"
        })
        .collect::<String>();
    let vectors_path = scratch.0.join("i64-vectors-canonical.txt");
    fs::write(&vectors_path, canonical_rows)
        .unwrap_or_else(|err| fail(format!("stage=oracle_matrix write: {err}")));
    let matrix_hash = sha256(&vectors_path);
    require(
        matrix_hash == VECTOR_MATRIX_SHA256,
        format!("stage=oracle_matrix cross-backend vector digest drift: {matrix_hash}"),
    );
    for level in ["0", "2"] {
        let binary = scratch.0.join(format!("tile-observer-o{level}"));
        let binary_str = binary.to_str().unwrap_or_else(|| fail("invalid binary path"));
        successful_at(
            "native_clang_link", "clang",
            &[
                &format!("-O{level}"),
                "-x", "ir", ir_str, "-x", "c", "-std=c11",
                "-Wall", "-Wextra", "-Werror", "-Wpedantic",
                c_str, "-o", binary_str,
            ],
        );
        checked_file(&binary);
        let binary_hash = sha256(&binary);
        for (index, row) in test_vectors.iter().copied().enumerate() {
            let expected = oracle(row)
                .unwrap_or_else(|| fail(format!("overflowing oracle vector {index}")));
            let decimal: Vec<String> = row.iter().map(i64::to_string).collect();
            let argv: Vec<&str> = decimal.iter().map(String::as_str).collect();
            let result = output(binary_str, &argv);
            require(
                result.status.success() && result.stderr.is_empty(),
                format!(
                    "stage=native_guest_runtime O{level} vector {index} failed: status={:?} stderr={}",
                    result.status,
                    String::from_utf8_lossy(&result.stderr)
                ),
            );
            let observed = String::from_utf8(result.stdout)
                .unwrap_or_else(|err| fail(format!("invalid guest UTF-8: {err}")));
            require(
                observed == format!("{expected}\n"),
                format!(
                    "stage=numerical_oracle O{level} vector {index} mismatch: \
                     expected {expected}, observed {:?}, inputs={row:?}",
                    observed
                ),
            );
            println!(
                "ATTO_HPC_I64_DIFF_VECTOR_PASS head={head} opt=O{level} \
                 vector={index} observed={expected} inputs={row:?}"
            );
        }
        println!(
            "ATTO_HPC_I64_DIFFERENTIAL_PASS head={head} tree={tree} \
             opt=O{level} vectors={} compiler_sha256={compiler_hash} \
             source_sha256={source_hash} matrix_sha256={matrix_hash} ir_sha256={ir_hash} \
             artifact_sha256={binary_hash} result_channel=native_i64_abi",
            test_vectors.len()
        );
        require(
            sha256(compiler) == compiler_hash
                && sha256(Path::new(FIXTURE)) == source_hash,
            "stage=post_execution_identity compiler or source was modified",
        );
    }
    require(
        successful("git", &["status", "--porcelain", "--", FIXTURE]).is_empty(),
        "source modified after guest execution",
    );
    println!(
        "ATTO_HPC_I64_DIFFERENTIAL_GATE_PASS head={head} tree={tree} \
         executions={} negative_cases=0 evidence_scope=compiled_guest_i64_abi",
        test_vectors.len() * 2
    );
}
