//! Small native CLI for the checked Wasm compiler.
//! The guest artifact is written only after full source/type checking.
use std::{env,fs,io::{self,Write},path::PathBuf,process,fs::OpenOptions};

fn run() -> Result<(), String> {
    let mut args = env::args_os().skip(1);
    let input = args.next().ok_or("usage: attoc <source.atto> [-o output.wasm]")?;
    if input == "--help" || input == "-h" {
        println!("usage: attoc <source.atto> [-o output.wasm]");
        return Ok(());
    }
    let input = PathBuf::from(input);
    let output = match args.next() {
        None => None,
        Some(flag) if flag == "-o" || flag == "--output" => {
            Some(PathBuf::from(args.next().ok_or("missing output path after -o")?))
        }
        Some(_) => return Err("usage: attoc <source.atto> [-o output.wasm]".into()),
    };
    if args.next().is_some() || output.as_ref().is_some_and(|p| p.as_os_str().is_empty()) {
        return Err("unexpected extra argument or empty output path".into());
    }
    let source = fs::read_to_string(&input)
        .map_err(|e| format!("E_INPUT: {}: {e}", input.display()))?;
    let bytes = atto_compiler::compile(&source).map_err(|e| e.to_string())?;
    // Preserve the existing frontend CLI contract from LLVM PRs #46/#57:
    // without -o stream binary Wasm directly to stdout, not to a disk file.
    let Some(output) = output else {
        return io::stdout().write_all(&bytes)
            .map_err(|e| format!("E_OUTPUT: stdout: {e}"));
    };
    // Create-new temp file in the destination directory: compile failures and
    // I/O failures cannot leave behind a new, partially-written guest artifact.
    let temp = output.with_extension(format!("wasm.partial.{}", process::id()));
    // A source compiler must not disclose staged guest bytes to unrelated
    // local users before atomic publication. On Unix create owner-only,
    // independently of a caller's permissive umask.
    let mut stage_options = OpenOptions::new();
    stage_options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        stage_options.mode(0o600);
    }
    let mut file = stage_options.open(&temp)
        .map_err(|e| format!("E_OUTPUT: {}: {e}", temp.display()))?;
    let written = file.write_all(&bytes).and_then(|_| file.sync_all());
    drop(file);
    if let Err(e) = written {
        let _ = fs::remove_file(&temp);
        return Err(format!("E_OUTPUT: {}: {e}", temp.display()));
    }
    // Create-only atomic publication: unlike rename, hard_link never
    // overwrites existing artifacts, symlinks, or the input source even
    // when a different process races with this compiler.
    let published = fs::hard_link(&temp, &output);
    let cleanup = fs::remove_file(&temp);
    if let Err(e) = published {
        return Err(format!("E_OUTPUT: {}: {e}", output.display()));
    }
    cleanup.map_err(|e| format!("E_OUTPUT: staging cleanup {}: {e}", temp.display()))?;
    Ok(())
}

fn main() {
    if let Err(e) = run() {
        eprintln!("{e}");
        process::exit(1);
    }
}
