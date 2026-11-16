//! Exact-source LLVM compiler -> Clang -> native OS guest parity candidates.
//! Neither Git blob checks nor host arithmetic substitute for guest execution.
use crate::{require, run, stderr, stdout, TempDir};
use std::{ffi::OsStr, fs, path::Path};

pub fn run_cases(compiler: &Path, clang: &Path, fixture_root: &Path) -> Result<(), String> {
    let temp = TempDir::new("ores-native-cast-parity-").map_err(|e| e.to_string())?;
    let ir = temp.path.join("candidate.ll");
    let executable = temp.path.join("candidate-native");
    let cases = [
        ("45-cast-int-literal.ores", "69cecd231d32bf70ea36424e59c932861d01fe44"),
        ("46-chained-cast-int.ores", "b088dd3f5e3114be4affbb4935582b58f010d75f"),
        ("47-grouped-cast-multiply.ores", "9ebc3f7bc230557fe2f2639172883bb2f4589464"),
        ("48-readonly-local-cast.ores", "87d0773504e0d22fc3cf1ee76755b36a2fc415c6"),
        ("49-const-typed-identity.ores", "bff7c6070008c5c6a8b3316e8a289f93d3f8c5b3"),
        ("50-runtime-const-helper.ores", "825767ec1ac57816591c7337ce5d499e64f85b66"),
    ];
    for (filename, expected_blob) in cases {
        let source = fixture_root.join(filename);
        let meta = fs::symlink_metadata(&source).map_err(|e| format!("{filename}: {e}"))?;
        require(meta.file_type().is_file(), format!("{filename}: source must be regular"))?;
        require(meta.len() > 0 && meta.len() < 8192, format!("{filename}: source size"))?;
        let blob = run(
            Path::new("git"),
            [OsStr::new("hash-object"), OsStr::new("--"), source.as_os_str()],
        )?;
        require(
            blob.status.success() && stdout(&blob).trim() == expected_blob,
            format!("{filename}: exact Git blob mismatch: {}", stderr(&blob)),
        )?;

        let compiled = run(compiler, [source.as_os_str()])?;
        require(
            compiled.status.success()
                && stdout(&compiled).starts_with("; ModuleID")
                && compiled.stderr.is_empty(),
            format!("{filename}: native compiler admission failed: {}", stderr(&compiled)),
        )?;
        fs::write(&ir, &compiled.stdout).map_err(|e| format!("{filename}: {e}"))?;
        for optimization in ["-O0", "-O2"] {
            let linked = run(
                clang,
                [
                    OsStr::new(optimization),
                    ir.as_os_str(),
                    OsStr::new("-o"),
                    executable.as_os_str(),
                ],
            )?;
            require(
                linked.status.success(),
                format!("{filename} {optimization}: clang failed: {}", stderr(&linked)),
            )?;
            let guest = run(&executable, std::iter::empty::<&str>())?;
            require(
                guest.status.code() == Some(42)
                    && guest.stdout.is_empty()
                    && guest.stderr.is_empty(),
                format!(
                    "{filename} {optimization}: native guest exit {:?} (expected 42), stderr={}",
                    guest.status.code(),
                    stderr(&guest)
                ),
            )?;
        }
    }

    let negative_fixtures = [
        // Main #1779 (shared Java/.NET .atto fixtures): a type-first scalar
        // (`int x = e;`) is implicitly const and needs a compile-time value.
        ("51-type-first-helper.ores", "2037906871e51c020153b21f47b59d741c717666", Some("E_NON_CONST_INITIALIZER")),
        ("52-type-first-reassign.ores", "1211ca68540a2d0d5a00c8e0251bd53df48553dd", None),
        ("53-type-first-type-mismatch.ores", "083ee4f3d6557f0d33d232d099a98e483a064eab", Some("requires int, got bool")),
        ("54-type-first-self-reference.ores", "7ab647ca316e1f4586495c02d63d9ebcd780206d", None),
        ("55-type-first-missing-initializer.ores", "47306c46bd9d1914cc46467d23c8b5b89060f1d1", None),
    ];
    for (filename, expected_blob, diagnostic) in negative_fixtures {
        let source = fixture_root.join(filename);
        let meta = fs::symlink_metadata(&source).map_err(|e| format!("{filename}: {e}"))?;
        require(meta.file_type().is_file(), format!("{filename}: source must be regular"))?;
        let blob = run(
            Path::new("git"),
            [OsStr::new("hash-object"), OsStr::new("--"), source.as_os_str()],
        )?;
        require(
            blob.status.success() && stdout(&blob).trim() == expected_blob,
            format!("{filename}: exact Git blob mismatch: {}", stderr(&blob)),
        )?;
        let rejected = run(compiler, [source.as_os_str()])?;
        let rejected_stderr = stderr(&rejected);
        require(
            !rejected.status.success()
                && rejected.stdout.is_empty()
                && rejected_stderr.starts_with("oreslang-llvmc: ")
                && diagnostic.map_or(true, |needle| rejected_stderr.contains(needle)),
            format!(
                "{filename}: expected compiler-owned rejection, status={:?}, stderr={}",
                rejected.status.code(),
                rejected_stderr
            ),
        )?;
    }

    for (name, source_text, diagnostic) in [
        (
            "int-as-bool",
            "pub fnc main():int{return 7 as bool;}\n",
            "impossible native cast",
        ),
        (
            "bool-as-int",
            "pub fnc main():int{return true as int;}\n",
            "impossible native cast",
        ),
    ] {
        let source = temp.path.join(format!("{name}.ores"));
        fs::write(&source, source_text).map_err(|e| e.to_string())?;
        let rejected = run(compiler, [source.as_os_str()])?;
        require(
            !rejected.status.success()
                && rejected.stdout.is_empty()
                && stderr(&rejected).contains(diagnostic),
            format!(
                "{name}: missing native compiler-owned negative cast diagnostic: {}",
                stderr(&rejected)
            ),
        )?;
    }
    println!(
        "LLVM checked i64 identity: six pinned positive .ores programs at -O0/-O2, five pinned immutable-default negatives, two negative casts"
    );
    Ok(())
}
