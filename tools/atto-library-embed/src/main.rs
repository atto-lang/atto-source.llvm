//! Deterministic compiler embedding from canonical AttoLang source files.
//! --check never rewrites; --write is explicit. Source Git blob IDs are pinned.
//! No source-to-source interpretation or guest execution occurs here.
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

struct Source {
    file: &'static str,
    output: &'static str,
    delimiter: &'static str,
    blob: &'static str,
}
const SOURCES: [Source; 6] = [
    Source { file: "stdlib/numeric.atto",
        output: "src/main/cpp/oreslang/frontend/GeneratedNumericAtto.inc",
        delimiter: "ATTO_NUMERIC", blob: "5ced991f1d49b3c2a60fdd72b9003c0a4d18cd72" },
    Source { file: "stdlib/alloc/int_buffer.atto",
        output: "src/main/cpp/oreslang/frontend/GeneratedIntBufferAtto.inc",
        delimiter: "ATTO_INT_BUFFER", blob: "25b3d4bc06a0c859aacadaf0795414027a076bdc" },
    Source { file: "stdlib/alloc/int_buffer_bundle.atto",
        output: "src/main/cpp/oreslang/frontend/GeneratedIntBufferBundleAtto.inc",
        delimiter: "ATTO_INT_BUNDLE", blob: "07c7083e099a18f1db7097f9639efc677070eb95" },
    Source { file: "stdlib/alloc/heap.atto",
        output: "src/main/cpp/oreslang/frontend/GeneratedHeapAtto.inc",
        delimiter: "ATTO_HEAP", blob: "b58c3365b9118d0ff6b3c383cf580df289683fa0" },
    Source { file: "stdlib/alloc/arena.atto",
        output: "src/main/cpp/oreslang/frontend/GeneratedArenaAtto.inc",
        delimiter: "ATTO_ARENA", blob: "ae18bb071331f7d8e4919b5ebc47d0402212721f" },
    Source { file: "stdlib/alloc/region.atto",
        output: "src/main/cpp/oreslang/frontend/GeneratedRegionAtto.inc",
        delimiter: "ATTO_REGION", blob: "bd47dfe3ee758e4b52b297944870c11406928b25" },
];
const MAX_BYTES: usize = 1024 * 1024;

fn render(spec: &Source, bytes: &[u8]) -> Result<Vec<u8>, String> {
    if bytes.is_empty() || bytes.len() > MAX_BYTES {
        return Err(format!("{}: source empty or exceeds 1 MiB", spec.file));
    }
    let source = std::str::from_utf8(bytes).map_err(|_| format!("{}: invalid UTF-8",spec.file))?;
    if source.starts_with('\u{feff}') || source.contains('\0') || source.contains('\r') {
        return Err(format!("{}: BOM/NUL/CR forbidden in canonical source",spec.file));
    }
    let terminator = format!("){}\"", spec.delimiter);
    if source.contains(&terminator) {
        return Err(format!("{}: C++ raw literal delimiter collision",spec.file));
    }
    Ok(format!("// Generated from {}; do not edit manually.\n// source Git blob SHA-1: {}\nR\"{}({}){}\"\n",
        spec.file, spec.blob, spec.delimiter, source, spec.delimiter).into_bytes())
}

fn regular_file(path: &Path) -> Result<(), String> {
    let meta = fs::symlink_metadata(path).map_err(|e|format!("{}: {e}",path.display()))?;
    if !meta.file_type().is_file() || meta.file_type().is_symlink() {
        return Err(format!("{}: must be a regular non-symlink file",path.display()));
    }
    Ok(())
}

fn check_blob(root: &Path, spec: &Source) -> Result<(), String> {
    let output = Command::new("git").current_dir(root).args(["hash-object","--",spec.file])
        .output().map_err(|e|format!("git hash-object {}: {e}",spec.file))?;
    if !output.status.success() { return Err(format!("git hash-object {} failed",spec.file)); }
    let actual=std::str::from_utf8(&output.stdout).map_err(|_|"git stdout not UTF-8")?.trim();
    if actual != spec.blob {
        return Err(format!("{}: canonical Git blob changed: {} != {}",spec.file,actual,spec.blob));
    }
    Ok(())
}

fn run(root: &Path, check: bool) -> Result<(), String> {
    for spec in &SOURCES {
        let input = root.join(spec.file);
        let output = root.join(spec.output);
        regular_file(&input)?;
        check_blob(root,spec)?;
        let bytes = fs::read(&input).map_err(|e|format!("{}: {e}",input.display()))?;
        let expected = render(spec,&bytes)?;
        if check {
            regular_file(&output)?;
            let observed=fs::read(&output).map_err(|e|format!("{}: {e}",output.display()))?;
            if observed!=expected {
                return Err(format!("{}: generated embed differs from pinned .atto",spec.output));
            }
        } else {
            if let Ok(meta)=fs::symlink_metadata(&output) {
                if !meta.file_type().is_file() || meta.file_type().is_symlink(){
                    return Err(format!("{}: refusing nonregular/symlink target",output.display()));
                }
            }
            fs::write(&output,expected).map_err(|e|format!("{}: {e}",output.display()))?;
        }
        println!("ATTO_LIBRARY_SOURCE|{}|git_blob={}|{}|guest=NOT_EXECUTED",
            spec.file,spec.blob,if check {"CHECKED"} else {"GENERATED"});
    }
    Ok(())
}

fn main() -> ExitCode {
    let args:Vec<_>=std::env::args().skip(1).collect();
    let check=match args.as_slice(){
        [a] if a=="--check"=>true,
        [a] if a=="--write"=>false,
        _=>{eprintln!("usage: atto-library-embed --check | --write");return ExitCode::FAILURE;}
    };
    let root=PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    match run(&root,check){
        Ok(())=>ExitCode::SUCCESS,
        Err(message)=>{eprintln!("ATTO_LIBRARY_SOURCE_REJECTED={message}");ExitCode::FAILURE}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn exact_raw_literal_roundtrip() {
        let spec=&SOURCES[0];
        let bytes=b"define module numeric as\nend\n";
        let rendered=render(spec,bytes).unwrap();
        let generated=String::from_utf8(rendered).unwrap();
        let opening=format!("R\"{}(",spec.delimiter);
        let tail=format!("){}\"",spec.delimiter);
        assert_eq!(generated.split_once(&opening).unwrap().1.split_once(&tail).unwrap().0,
            std::str::from_utf8(bytes).unwrap());
    }
    #[test] fn mutation_controls_reject_invalid_canonical_bytes() {
        let spec=&SOURCES[0];
        for bytes in [b"".as_slice(),b"\xff".as_slice(),b"ok\0".as_slice(),b"\xef\xbb\xbfok".as_slice(),
            b"bad\r\n".as_slice(),b")ATTO_NUMERIC\"".as_slice()] {
            assert!(render(spec,bytes).is_err());
        }
        assert!(render(spec,&vec![b'a'; MAX_BYTES+1]).is_err());
    }
    #[test] fn render_is_deterministic() {
        let bytes=b"define module numeric as\nend\n";
        assert_eq!(render(&SOURCES[0],bytes).unwrap(),render(&SOURCES[0],bytes).unwrap());
        let a=render(&SOURCES[0],bytes).unwrap();
        let b=render(&SOURCES[0],b"define module numeric as\n// mutated\nend\n").unwrap();
        assert_ne!(a,b);
    }
}
