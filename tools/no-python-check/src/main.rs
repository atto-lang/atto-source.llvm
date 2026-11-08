use std::{
    env, fs,
    path::Path,
    process::{self, Command},
};

fn forbidden_path(path: &str) -> bool {
    let p = path.to_ascii_lowercase();
    let file = p.rsplit('/').next().unwrap_or(&p);
    p.ends_with(".py")
        || p.ends_with(".pyi")
        || p.ends_with(".pyw")
        || p.ends_with(".ipynb")
        || file == "pipfile"
        || file == "pipfile.lock"
        || file == "poetry.lock"
        || file == "pyproject.toml"
        || file == "uv.lock"
        || file == "setup.py"
        || file == "tox.ini"
        || file == ".python-version"
        || file == "pdm.lock"
        || (file.starts_with("requirements") && file.ends_with(".txt"))
}

fn is_execution_surface(path: &str) -> bool {
    let p = path.to_ascii_lowercase();
    let file = p.rsplit('/').next().unwrap_or(&p);
    (p.starts_with(".github/workflows/") && (p.ends_with(".yaml") || p.ends_with(".yml")))
        || (p.starts_with(".github/actions/") && (p.ends_with(".yaml") || p.ends_with(".yml")))
        || file == "makefile"
        || file == "justfile"
        || p.ends_with(".sh")
        || p.ends_with(".bash")
        || p.ends_with(".zsh")
        || file == "cmakelists.txt"
        || file == "dockerfile"
        || p.ends_with(".cmake")
        || p.ends_with(".rs")
        || p.ends_with(".mjs")
        || p.ends_with(".js")
        || p.ends_with(".md")
        || p.ends_with(".ps1")
        || p.ends_with(".cmd")
}

fn forbidden_execution_line(line: &str) -> bool {
    let l = line.trim().to_ascii_lowercase();
    if l.starts_with('#') {
        return false;
    }
    [
        "actions/setup-python@",
        "python -m ",
        "python3 -m ",
        "python -c ",
        "python3 -c ",
        "python ",
        "python3 ",
        "pypy ",
        "pypy3 ",
        "pip install",
        "pip3 install",
        "uv run ",
        "pytest ",
        "poetry run ",
        "pdm run ",
    ]
    .iter()
    .any(|needle| l.contains(needle))
}

fn python_shebang(body: &str) -> bool {
    let first = body
        .lines()
        .next()
        .unwrap_or("")
        .trim()
        .to_ascii_lowercase();
    first.starts_with("#!")
        && (first.contains("/python")
            || first.contains("env python")
            || first.contains("env -s python")
            || first.contains("/pypy")
            || first.contains("env pypy"))
}

fn violations(path: &str, body: &str) -> Vec<String> {
    let mut messages = Vec::new();
    if forbidden_path(path) {
        messages
            .push("Python source, notebook, dependency manifest, or environment file".to_string());
    }
    if python_shebang(body) {
        messages.push("Python/PyPy shebang".to_string());
    }
    if is_execution_surface(path) {
        for (index, line) in body.lines().enumerate() {
            if forbidden_execution_line(line) {
                messages.push(format!("Python tooling invocation at line {}", index + 1));
            }
        }
    }
    messages
}

// The zero-debt scanner is still tools/check_no_python.rs.  This gate freezes
// the exact legacy tree and forbids NEW Python; it does NOT certify migration.
// Updating the baseline requires independent owner review.
const LEGACY_BASELINE: &str = "443d2a1e150557f158fc5cfb457bde204f106dd2";

fn git(root: &str, args: &[&str]) -> Result<Vec<u8>, String> {
    let output = Command::new("git")
        .args(["-C", root])
        .args(args)
        .output()
        .map_err(|err| format!("git invocation failed: {err}"))?;
    if !output.status.success() {
        return Err(format!(
            "git {} failed: {}",
            args.join(" "),
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    Ok(output.stdout)
}

fn changed_paths(bytes: &[u8]) -> Result<Vec<String>, String> {
    if !bytes.is_empty() && bytes.last() != Some(&0) {
        return Err("git -z paths are not NUL-terminated".to_string());
    }
    bytes
        .split(|x| *x == 0)
        .filter(|x| !x.is_empty())
        .map(|raw| {
            let name = std::str::from_utf8(raw).map_err(|_| "non-UTF8 path".to_string())?;
            if name.starts_with('/')
                || name.contains('\n')
                || name.contains('\r')
                || name.contains('\t')
                || name.split('/').any(|part| part == ".." || part.is_empty())
            {
                return Err("unsafe changed path".to_string());
            }
            Ok(name.to_string())
        })
        .collect()
}

fn is_added_forbidden_line(line: &str) -> bool {
    let Some(added) = line.strip_prefix('+') else {
        return false;
    };
    !line.starts_with("+++") && !violations("added-policy-probe.sh", added).is_empty()
}

fn scan_migration(root: &str, baseline: &str) -> Result<Vec<String>, String> {
    if baseline.len() != 40 || !baseline.bytes().all(|c| c.is_ascii_hexdigit()) {
        return Err("frozen legacy base must be exact SHA".to_string());
    }
    git(root, &["merge-base", "--is-ancestor", baseline, "HEAD"])?;
    git(root, &["diff", "--quiet", "HEAD", "--"])?;
    git(root, &["diff", "--cached", "--quiet", "HEAD", "--"])?;
    let names = changed_paths(&git(
        root,
        &["diff", "--name-only", "--no-renames", "-z", baseline, "HEAD", "--"],
    )?)?;
    let mut findings = Vec::new();
    for name in names {
        let file = Path::new(root).join(&name);
        let metadata = match fs::symlink_metadata(file) {
            Ok(metadata) => metadata,
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => {
                // Deleting old Python files or commands reduces legacy debt.
                continue;
            }
            Err(err) => return Err(format!("cannot inspect changed {name}: {err}")),
        };
        if metadata.file_type().is_symlink() {
            return Err(format!("new/modified symlink disallowed in Python migration: {name}"));
        }
        if forbidden_path(&name) {
            findings.push(format!("{name}: new or modified Python artifact"));
            continue;
        }
        // The scanners are the policy implementation itself, not a Python
        // execution surface.  They require CODEOWNER security review.
        if matches!(
            name.as_str(),
            "tools/check_no_python.rs" | "tools/no-python-check/src/main.rs"
        ) {
            continue;
        }
        if !is_execution_surface(&name) {
            continue;
        }
        if !metadata.is_file() {
            return Err(format!("changed execution surface is not regular: {name}"));
        }
        let patch = git(
            root,
            &[
                "diff",
                "--no-ext-diff",
                "--no-renames",
                "--unified=0",
                "--text",
                baseline,
                "HEAD",
                "--",
                &name,
            ],
        )?;
        let text = String::from_utf8(patch)
            .map_err(|_| format!("invalid UTF8 in execution change: {name}"))?;
        for line in text.lines().filter(|line| is_added_forbidden_line(line)) {
            findings.push(format!("{name}: added Python execution: {line}"));
        }
    }
    Ok(findings)
}

fn main() {
    let root = env::args().nth(1).unwrap_or_else(|| ".".to_string());
    match scan_migration(&root, LEGACY_BASELINE) {
        Ok(findings) if findings.is_empty() => {
            println!(
                "NO_NEW_PYTHON_REGRESSIONS=PASS; BASELINE={LEGACY_BASELINE}; \
                 FULL_RUST_ONLY_MIGRATION=NOT_COMPLETE"
            );
        }
        Ok(findings) => {
            for finding in &findings {
                eprintln!("Python legacy freeze violation: {finding}");
            }
            process::exit(1);
        }
        Err(error) => {
            eprintln!("Python migration gate FAIL_CLOSED: {error}");
            process::exit(2);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blocks_python_assets_and_manifests() {
        for path in [
            "tool/doit.py",
            "tool/doit.pyw",
            "types/x.pyi",
            "tests/a.ipynb",
            "pyproject.toml",
            "requirements-dev.txt",
            "Pipfile.lock",
            ".python-version",
            "tox.ini",
        ] {
            assert!(forbidden_path(path), "must reject {path}");
        }
        assert!(!forbidden_path("README.md"));
        assert!(!forbidden_path("src/main.rs"));
    }

    #[test]
    fn recognizes_execution_surfaces() {
        assert!(is_execution_surface(".github/workflows/ci.yml"));
        assert!(is_execution_surface(".github/actions/check/action.yaml"));
        assert!(is_execution_surface("scripts/check.sh"));
        assert!(is_execution_surface("Makefile"));
        assert!(is_execution_surface("justfile"));
        assert!(!is_execution_surface("docs/policy.md"));
    }

    #[test]
    fn blocks_python_ci_invocations() {
        assert!(forbidden_execution_line("- uses: actions/setup-python@v5"));
        assert!(forbidden_execution_line("- run: python3 -m pytest"));
        assert!(forbidden_execution_line("pip install -r requirements.txt"));
        assert!(forbidden_execution_line("uv run tool.py"));
        assert!(!forbidden_execution_line("- run: cargo test"));
    }

    #[test]
    fn blocks_extensionless_python_scripts() {
        assert!(python_shebang("#!/usr/bin/env python3\nprint('x')"));
        assert!(python_shebang("#!/usr/bin/python\nprint('x')"));
        assert!(python_shebang("#!/usr/bin/env pypy3\nprint('x')"));
        assert!(!python_shebang("#!/usr/bin/env bash\necho ok"));
    }


    #[test]
    fn frozen_migration_delta_parsing_rejects_unsafe_names() {
        assert_eq!(
            changed_paths(b"tests/new.rs\0docs/readme.md\0").unwrap(),
            vec!["tests/new.rs", "docs/readme.md"]
        );
        for bad in [b"file".as_slice(), b"../escape\0", b"tab\tfile\0", b"x\xff\0"] {
            assert!(changed_paths(bad).is_err());
        }
    }

    #[test]
    fn new_python_execution_is_denied_but_unchanged_legacy_is_not_duplicated() {
        assert!(is_added_forbidden_line("+  - run: python3 -m pytest"));
        assert!(is_added_forbidden_line("+#!/usr/bin/env python3"));
        assert!(is_added_forbidden_line("+  - uses: actions/setup-python@v5"));
        assert!(!is_added_forbidden_line("   run: python3 -m pytest"));
        assert!(!is_added_forbidden_line("-  - run: python3 -m pytest"));
        assert!(!is_added_forbidden_line("+++ b/scripts/test.sh"));
        assert!(!is_added_forbidden_line("+  # Python migration is pending"));
        assert!(forbidden_path("tests/legacy.py"));
        assert!(is_execution_surface("CMakeLists.txt"));
        assert!(is_execution_surface(".github/workflows/ci.yml"));
    }

    #[test]
    fn prose_is_not_treated_as_execution() {
        let body = "Python is prohibited; replace Python tooling with Rust.";
        assert!(violations("README.md", body).is_empty());
    }
}
