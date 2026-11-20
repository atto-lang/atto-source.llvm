//! Pointer-width gate for the native actor heap runtime.
//!
//! Builds every header-level actor heap / ref32 / carrier test with a given C
//! and C++ compiler and flag set (for example `-m32` on an x86_64 runner, or an
//! ARMv7 cross compiler plus `--runner qemu-arm`), runs each binary, and fails
//! if any test fails to build or run. The full CMake tree needs LLVM libraries
//! for the target, which 32-bit hosts do not have; these tests do not.
//!
//! The same source must pass on 64-bit and 32-bit hosts: heap metadata is
//! 32-bit on every host, and ref32 words are identical across widths.
//!
//! Usage:
//!   rustc --edition=2021 -D warnings tests/native_heap_pointer_width_gate.rs -o gate
//!   ./gate --root . --cc gcc --cxx g++ --flag -m32 --expect-pointer-bits 32
//!   ./gate --root . --cc arm-linux-gnueabihf-gcc --cxx arm-linux-gnueabihf-g++ \
//!          --runner qemu-arm --runner -L --runner /usr/arm-linux-gnueabihf \
//!          --expect-pointer-bits 32

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

const RUNTIME: &str = "src/main/cpp/oreslang/runtime";

struct HeapTest {
    name: &'static str,
    sources: &'static [&'static str],
    /// Linker --wrap interception of calloc/free (GNU ld / lld only).
    link_wrap: bool,
}

const TESTS: &[HeapTest] = &[
    HeapTest { name: "actor_heap", sources: &["tests/actor_heap_test.cpp"], link_wrap: false },
    HeapTest { name: "actor_owned_heap", sources: &["tests/actor_owned_heap_test.cpp"], link_wrap: false },
    HeapTest {
        name: "actor_owned_i64_heap",
        sources: &["tests/actor_owned_heap_lifecycle_test.cpp", "@/ActorOwnedI64Heap.c"],
        link_wrap: true,
    },
    HeapTest {
        name: "actor_heap_access_trace",
        sources: &["tests/actor_heap_access_trace_test.c", "@/ActorOwnedI64Heap.c"],
        link_wrap: false,
    },
    HeapTest {
        name: "actor_graph_heap",
        sources: &["tests/actor_owned_graph_heap_test.cpp", "@/HungryCarrier.cpp", "@/ParkingSignal.cpp"],
        link_wrap: false,
    },
    HeapTest {
        name: "hungry_carrier",
        sources: &["tests/hungry_carrier_test.cpp", "@/HungryCarrier.cpp", "@/ParkingSignal.cpp"],
        link_wrap: false,
    },
    HeapTest { name: "actor_graph_reverse_drop", sources: &["tests/actor_graph_reverse_drop_test.cpp"], link_wrap: false },
    HeapTest { name: "actor_heap_borrow_gate", sources: &["tests/actor_heap_borrow_gate_lifetime_test.cpp"], link_wrap: false },
    HeapTest { name: "actor_compact_field_store", sources: &["tests/actor_compact_field_store_test.cpp"], link_wrap: false },
    HeapTest { name: "managed_ref", sources: &["tests/managed_ref_test.cpp"], link_wrap: false },
    HeapTest { name: "managed_ref_layout_plan", sources: &["tests/managed_ref_layout_plan_test.cpp"], link_wrap: false },
    HeapTest { name: "native_actor_bytes_map", sources: &["tests/native_actor_bytes_map_test.cpp"], link_wrap: false },
    HeapTest { name: "native_region_registry", sources: &["tests/native_region_registry_test.cpp"], link_wrap: false },
    HeapTest { name: "trusted_pointer_window", sources: &["tests/trusted_pointer_window_test.cpp"], link_wrap: false },
    HeapTest { name: "owned_scalar_state", sources: &["tests/actor_owned_scalar_state_test.cpp"], link_wrap: false },
    HeapTest { name: "dop_layout", sources: &["tests/dop_layout_test.cpp"], link_wrap: false },
    HeapTest {
        name: "shard_layout",
        sources: &["tests/native_actor_shard_layout_test.cpp", "@/NativeActorScheduler.c"],
        link_wrap: false,
    },
    HeapTest { name: "shard_snapshot", sources: &["tests/native_actor_shard_snapshot_unit_test.cpp"], link_wrap: false },
];

/// Prints the host pointer width, so a misconfigured `-m32` cannot pass as 64-bit.
const WIDTH_PROBE: &str = "#include <cstdio>\nint main(){std::printf(\"%zu\\n\", sizeof(void*) * 8);}\n";

struct Options {
    root: PathBuf,
    cc: String,
    cxx: String,
    flags: Vec<String>,
    runner: Vec<String>,
    expect_bits: Option<u32>,
    no_link_wrap: bool,
}

fn parse() -> Options {
    let mut o = Options {
        root: PathBuf::from("."),
        cc: "cc".into(),
        cxx: "c++".into(),
        flags: Vec::new(),
        runner: Vec::new(),
        expect_bits: None,
        no_link_wrap: false,
    };
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut value = || args.next().unwrap_or_else(|| fail(&format!("{arg} requires a value")));
        match arg.as_str() {
            "--root" => o.root = PathBuf::from(value()),
            "--cc" => o.cc = value(),
            "--cxx" => o.cxx = value(),
            "--flag" => o.flags.push(value()),
            "--runner" => o.runner.push(value()),
            "--expect-pointer-bits" => {
                o.expect_bits = Some(value().parse().unwrap_or_else(|_| fail("bad pointer bits")))
            }
            "--no-link-wrap" => o.no_link_wrap = true,
            _ => fail(&format!("unknown argument: {arg}")),
        }
    }
    o
}

fn fail(why: &str) -> ! {
    eprintln!("FAIL native heap pointer-width gate: {why}");
    process::exit(1);
}

fn run(program: &str, args: &[String], cwd: &Path) -> Result<String, String> {
    let out = Command::new(program)
        .args(args)
        .current_dir(cwd)
        .output()
        .map_err(|e| format!("{program}: {e}"))?;
    let text = format!(
        "{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    if out.status.success() {
        Ok(text)
    } else {
        Err(format!("{program} {} -> {}\n{text}", args.join(" "), out.status))
    }
}

fn execute(o: &Options, binary: &Path) -> Result<String, String> {
    let bin = binary.to_string_lossy().into_owned();
    match o.runner.split_first() {
        None => run(&bin, &[], &o.root),
        Some((program, rest)) => {
            let mut args = rest.to_vec();
            args.push(bin);
            run(program, &args, &o.root)
        }
    }
}

fn build(o: &Options, out: &Path, test: &HeapTest) -> Result<PathBuf, String> {
    let include = format!("-I{}", o.root.join("src/main/cpp").display());
    let warn = ["-Wall", "-Wextra", "-Wpedantic", "-Werror"].map(String::from);
    let mut objects = Vec::new();
    for source in test.sources {
        let path = match source.strip_prefix("@/") {
            Some(file) => format!("{RUNTIME}/{file}"),
            None => (*source).to_string(),
        };
        let stem = Path::new(&path).file_name().unwrap().to_string_lossy().into_owned();
        let object = out.join(format!("{}.{stem}.o", test.name));
        let c = path.ends_with(".c");
        let mut args = vec![if c { "-std=c11" } else { "-std=c++17" }.to_string(), "-O1".into(), "-g".into()];
        if c {
            // POSIX/GNU affinity and clock APIs used by the native scheduler.
            args.push("-D_GNU_SOURCE".into());
        }
        args.extend(warn.iter().cloned());
        args.extend(o.flags.iter().cloned());
        if test.link_wrap && o.no_link_wrap {
            args.push("-DORES_HEAP_NO_LINK_WRAP=1".into());
        }
        args.push(include.clone());
        args.extend(["-c".into(), path.clone(), "-o".into(), object.to_string_lossy().into_owned()]);
        run(if c { &o.cc } else { &o.cxx }, &args, &o.root)?;
        objects.push(object.to_string_lossy().into_owned());
    }
    let binary = out.join(test.name);
    let mut args = o.flags.clone();
    args.extend(objects);
    args.push("-pthread".into());
    if test.link_wrap && !o.no_link_wrap {
        args.extend(["-Wl,--wrap=calloc".into(), "-Wl,--wrap=free".into()]);
    }
    args.extend(["-o".into(), binary.to_string_lossy().into_owned()]);
    run(&o.cxx, &args, &o.root)?;
    Ok(binary)
}

fn main() {
    let o = parse();
    let out = env::temp_dir().join(format!("oreslang-heap-width-gate-{}", process::id()));
    fs::create_dir_all(&out).unwrap_or_else(|e| fail(&format!("{}: {e}", out.display())));

    // 1. Prove which pointer width these flags actually produce.
    let probe_src = out.join("width.cpp");
    fs::write(&probe_src, WIDTH_PROBE).unwrap_or_else(|e| fail(&e.to_string()));
    let probe = out.join("width");
    let mut args = o.flags.clone();
    args.extend([probe_src.to_string_lossy().into_owned(), "-o".into(), probe.to_string_lossy().into_owned()]);
    if let Err(e) = run(&o.cxx, &args, &o.root) {
        fail(&format!("width probe did not build: {e}"));
    }
    let bits = execute(&o, &probe).unwrap_or_else(|e| fail(&format!("width probe did not run: {e}")));
    let bits: u32 = bits.trim().parse().unwrap_or_else(|_| fail(&format!("width probe printed {bits:?}")));
    if let Some(expected) = o.expect_bits {
        if bits != expected {
            fail(&format!("flags produce {bits}-bit pointers, expected {expected}"));
        }
    }
    println!("host pointer width: {bits} bits");

    // 2. Build and run every heap test; never skip one silently.
    let mut failures = Vec::new();
    for test in TESTS {
        let outcome = build(&o, &out, test).and_then(|binary| execute(&o, &binary));
        match outcome {
            Ok(_) => println!("PASS {bits}-bit {}", test.name),
            Err(e) => {
                println!("FAIL {bits}-bit {}", test.name);
                eprintln!("{e}");
                failures.push(test.name);
            }
        }
    }
    let _ = fs::remove_dir_all(&out);
    if !failures.is_empty() {
        fail(&format!("{} of {} tests failed: {}", failures.len(), TESTS.len(), failures.join(", ")));
    }
    println!("PASS native actor heap runtime on {bits}-bit host: {} tests", TESTS.len());
}
