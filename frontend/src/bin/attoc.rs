//! Small native CLI for the checked Wasm compiler.
//! The guest artifact is written only after full source/type checking.
use std::{env,fs,io::Write,path::PathBuf,process,fs::OpenOptions};

fn run() -> Result<(), String> {
    let mut args = env::args_os().skip(1);
    let input = args.next().ok_or("usage: attoc <source.atto> [-o output.wasm]")?;
    if input == "--help" || input == "-h" {
        println!("usage: attoc <source.atto> [-o output.wasm]");
        return Ok(());
    }
    let input = PathBuf::from(input);
    let output = match args.next() {
        None => input.with_extension("wasm"),
        Some(flag) if flag == "-o" || flag == "--output" => {
            PathBuf::from(args.next().ok_or("missing output path after -o")?)
        }
        Some(_) => return Err("usage: attoc <source.atto> [-o output.wasm]".into()),
    };
    if args.next().is_some() || output.as_os_str().is_empty() {
        return Err("unexpected extra argument or empty output path".into());
    }
    let source = fs::read_to_string(&input)
        .map_err(|e| format!("E_INPUT: {}: {e}", input.display()))?;
    let bytes = atto_compiler::compile(&source).map_err(|e| e.to_string())?;
    // Create-new temp file in the destination directory: compile failures and
    // I/O failures cannot leave behind a new, partially-written guest artifact.
    let temp = output.with_extension(format!("wasm.partial.{}", process::id()));
    let mut file = OpenOptions::new().write(true).create_new(true).open(&temp)
        .map_err(|e| format!("E_OUTPUT: {}: {e}", temp.display()))?;
    let written = file.write_all(&bytes).and_then(|_| file.sync_all());
    drop(file);
    if let Err(e) = written {
        let _ = fs::remove_file(&temp);
        return Err(format!("E_OUTPUT: {}: {e}", temp.display()));
    }
    if let Err(e) = fs::rename(&temp, &output) {
        let _ = fs::remove_file(&temp);
        return Err(format!("E_OUTPUT: {}: {e}", output.display()));
    }
    Ok(())
}

fn main() {
    if let Err(e) = run() {
        eprintln!("{e}");
        process::exit(1);
    }
}
