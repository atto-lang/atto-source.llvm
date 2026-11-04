#!/usr/bin/env python3
"""Lower tenant source or Wasm to a loadable guest library for an in-process tenant VM.

    .atto -> LLVM IR -> wasm32 -> ADMIT -> wasm2c -> clang -> guest library + manifest

This tool, not the host, is the security boundary: it is what makes the addressing rule true
of tenant code. It fails closed. Anything it cannot positively recognize is refused, and tenant
input cannot reach a tool without a time and memory limit, because it would otherwise be able
to stall the service or exhaust memory while being compiled.

Run it on a machine or under a sandbox you are willing to lose: clang and wasm2c parse data the
tenant chose. Its output is pinned by digest, and only that digest should be loaded.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import resource
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

WASM_MAGIC = b"\0asm\x01\0\0\0"
PAGE = 65536
FUNCREF = 0x70
VALUE_TYPES = {0x7F, 0x7E, 0x7D, 0x7C}  # i32, i64, f32, f64. No v128, no references.

# Admission limits. Lowering time grows much faster than module size: a 2 MiB module with large
# functions took clang -O2 4m37s, so module and function sizes are bounded, not just the clock.
MAX_MODULE_BYTES = 1 << 20
MAX_FUNCTIONS = 20000
MAX_FUNCTION_BYTES = 128 << 10
MAX_LOCALS = 4096
MAX_TABLE_SLOTS = 4096
MAX_MEMORY_BYTES = 256 << 20
MAX_GLOBALS = 1024
MAX_FRAME_BYTES = 512 << 10  # half of the carrier's guard, so one frame can never step over it

# Leaf functions lowered code may call. Each is reentrant and takes no lock, so a kill landing
# inside one leaves nothing behind. Anything else in the library's imports refuses the build.
ALLOWED_IMPORTS = {
    "memcpy", "memmove", "memset", "memcmp", "__memcpy_chk", "__memmove_chk", "__memset_chk", "bzero",
    "ceil", "ceilf", "floor", "floorf", "trunc", "truncf", "nearbyint", "nearbyintf", "rint", "rintf",
    "sqrt", "sqrtf", "fabs", "fabsf", "copysign", "copysignf", "fmin", "fminf", "fmax", "fmaxf",
    # Dynamic-linker and C-runtime bookkeeping, never reached by lowered code.
    "__cxa_finalize", "__cxa_atexit", "dyld_stub_binder", "__gmon_start__", "_ITM_deregisterTMCloneTable",
    "_ITM_registerTMCloneTable",
}
ALLOWED_WRITABLE_SYMBOLS = {"g_host"}

# The only functions a module may import, each with exactly this signature (0x7E is i64). The adapter
# defines them for this signature, and C linking would not notice a module that declared another.
HOST_IMPORTS = {
    "oreslang_vm_spawn": ((0x7E,), (0x7E,)),
    "oreslang_vm_send": ((0x7E, 0x7E), ()),
    "oreslang_vm_stop": ((0x7E,), ()),
}

# The only proposals lowered code may use. Each wabt release turns more on by default (memory64,
# multi-memory and tail calls among them), and some of those would break the addressing rule. So
# everything else a given tool can disable is disabled, including features it adds later.
ALLOWED_FEATURES = {"mutable-globals", "saturating-float-to-int", "sign-extension", "multi-value"}


def locked_features(tool):
    help_text = subprocess.run([tool, "--help"], capture_output=True, text=True, timeout=30).stdout
    flags = sorted(set(re.findall(r"--disable-([a-z0-9-]+)", help_text)))
    if "memory64" not in flags and subprocess.run([tool, "--help"], capture_output=True, text=True, timeout=30).returncode != 0:
        raise Refused(f"cannot read the features of {tool}")
    return [f"--disable-{name}" for name in flags if name in FEATURE_NAMES and name not in ALLOWED_FEATURES]


# wasm2c's other --disable- options are not language features.
FEATURE_NAMES = {"exceptions", "mutable-globals", "saturating-float-to-int", "sign-extension", "simd", "threads",
                 "function-references", "multi-value", "tail-call", "bulk-memory", "reference-types", "annotations",
                 "code-metadata", "gc", "memory64", "multi-memory", "extended-const", "relaxed-simd", "custom-page-sizes",
                 "compact-imports", "wide-arithmetic", "stack-switching", "custom-descriptors"}


class Refused(Exception):
    """The input is not something this pipeline will lower."""


class Reader:
    def __init__(self, data, position=0, end=None):
        self.data = data
        self.position = position
        self.end = len(data) if end is None else end

    def byte(self):
        if self.position >= self.end:
            raise Refused("truncated module")
        value = self.data[self.position]
        self.position += 1
        return value

    def u32(self):
        result, shift = 0, 0
        for _ in range(5):
            b = self.byte()
            result |= (b & 0x7F) << shift
            if not b & 0x80:
                if result >= 1 << 32:
                    raise Refused("LEB128 value out of range")
                return result
            shift += 7
        raise Refused("LEB128 value too long")

    def bytes(self, count):
        if count > self.end - self.position:
            raise Refused("truncated module")
        value = self.data[self.position:self.position + count]
        self.position += count
        return value

    def name(self):
        raw = self.bytes(self.u32())
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise Refused("name is not UTF-8") from error

    def done(self):
        return self.position >= self.end


def read_limits(r, what):
    flags = r.byte()
    if flags not in (0, 1):
        # Bit 1 is shared memory, bit 2 is 64-bit addressing. Either would break the rule's premise.
        raise Refused(f"{what}: limits flags {flags:#x} are not allowed (shared or 64-bit)")
    minimum = r.u32()
    maximum = r.u32() if flags & 1 else None
    return minimum, maximum


def admit(data, required_exports, memory_bytes):
    """Checks structure the lowering depends on. Instruction validity is wasm2c's job after this."""
    if len(data) > MAX_MODULE_BYTES:
        raise Refused(f"module is {len(data)} bytes, limit {MAX_MODULE_BYTES}")
    if data[:8] != WASM_MAGIC:
        raise Refused("not a Wasm 1.0 binary")
    r = Reader(data, 8)
    seen, last_order = set(), 0
    memory_imported = False
    table_slots = 0
    types = []
    imported_functions = set()
    exports = {}
    function_count = 0
    # The order Wasm requires, with data count (12) between element (9) and code (10).
    order = {1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 9: 8, 12: 9, 10: 10, 11: 11}
    while not r.done():
        section = r.byte()
        size = r.u32()
        body = Reader(data, r.position, r.position + size)
        if body.end > len(data):
            raise Refused("section runs past the end of the module")
        r.position += size
        if section == 0:
            body.name()
            continue
        if section not in order:
            raise Refused(f"section {section} is not allowed")  # start, tags, anything newer
        if section in seen or order[section] <= last_order:
            raise Refused(f"section {section} is duplicated or out of order")
        seen.add(section)
        last_order = order[section]
        if section == 1:
            for _ in range(body.u32()):
                if body.byte() != 0x60:
                    raise Refused("a type that is not a function type")
                params = tuple(body.byte() for _ in range(body.u32()))
                results = tuple(body.byte() for _ in range(body.u32()))
                types.append((params, results))
        elif section == 2:
            for _ in range(body.u32()):
                module, name, kind = body.name(), body.name(), body.byte()
                if module == "env" and kind == 0 and name in HOST_IMPORTS:
                    index = body.u32()
                    if index >= len(types) or types[index] != HOST_IMPORTS[name]:
                        raise Refused(f"import {name} does not have its required signature")
                    if name in imported_functions:
                        raise Refused(f"duplicate import {name}")
                    imported_functions.add(name)
                    continue
                if (module, name, kind) != ("env", "memory", 2):
                    raise Refused(f"import {module}.{name} of kind {kind} is not allowed")
                if memory_imported:
                    raise Refused("more than one memory")
                minimum, maximum = read_limits(body, "memory")
                if minimum * PAGE > memory_bytes:
                    raise Refused(f"memory needs {minimum * PAGE} bytes, this build gives an actor {memory_bytes}")
                if maximum is not None and maximum * PAGE > memory_bytes:
                    raise Refused("declared memory maximum exceeds what this build gives an actor")
                memory_imported = True
        elif section == 3:
            function_count = body.u32()
            if function_count > MAX_FUNCTIONS:
                raise Refused(f"{function_count} functions, limit {MAX_FUNCTIONS}")
        elif section == 4:
            count = body.u32()
            if count > 1:
                raise Refused("more than one table")
            for _ in range(count):
                if body.byte() != FUNCREF:
                    raise Refused("table of something other than funcref")
                minimum, maximum = read_limits(body, "table")
                if maximum is None or maximum != minimum:
                    raise Refused("table must have a fixed size, nothing here can grow it")
                if minimum > MAX_TABLE_SLOTS:
                    raise Refused(f"table of {minimum} slots, limit {MAX_TABLE_SLOTS}")
                table_slots = minimum
        elif section == 5:
            # Linear memory must be the actor's region, which only an import can make it.
            if body.u32() != 0:
                raise Refused("module defines its own memory; it must import env.memory")
        elif section == 6:
            count = body.u32()
            if count > MAX_GLOBALS:
                raise Refused(f"{count} globals, limit {MAX_GLOBALS}")
        elif section == 7:
            for _ in range(body.u32()):
                name, kind = body.name(), body.byte()
                index = body.u32()
                if kind != 0:
                    raise Refused(f"export {name} of kind {kind}: only functions may be exported")
                if name in exports:
                    raise Refused(f"duplicate export {name}")
                exports[name] = index
        elif section == 10:
            count = body.u32()
            if count != function_count:
                raise Refused("code and function sections disagree")
            for _ in range(count):
                size = body.u32()
                if size > MAX_FUNCTION_BYTES:
                    raise Refused(f"function body of {size} bytes, limit {MAX_FUNCTION_BYTES}")
                code = Reader(data, body.position, body.position + size)
                if code.end > body.end:
                    raise Refused("function runs past its section")
                locals_total = 0
                for _ in range(code.u32()):
                    run = code.u32()
                    if code.byte() not in VALUE_TYPES:
                        raise Refused("local of a type this build does not allow")
                    locals_total += run
                    if locals_total > MAX_LOCALS:
                        raise Refused(f"more than {MAX_LOCALS} locals in one function")
                body.position += size
    if not memory_imported:
        raise Refused("module does not import env.memory")
    missing = [name for name in required_exports if name not in exports]
    if missing:
        raise Refused(f"missing required exports: {', '.join(missing)}")
    return {"table_slots": table_slots, "functions": function_count, "exports": sorted(exports)}


def limit_child(cpu_seconds, memory_bytes):
    def apply():
        resource.setrlimit(resource.RLIMIT_CPU, (cpu_seconds, cpu_seconds + 2))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_FSIZE, (256 << 20, 256 << 20))
        if platform.system() == "Linux":  # macOS does not enforce an address-space limit
            resource.setrlimit(resource.RLIMIT_AS, (memory_bytes, memory_bytes))
    return apply


def run(argv, args, stage, stdin=None):
    try:
        result = subprocess.run(argv, input=stdin, capture_output=True, timeout=args.tool_seconds * 2,
                                preexec_fn=limit_child(args.tool_seconds, args.tool_memory_bytes),
                                env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"})
    except subprocess.TimeoutExpired as error:
        raise Refused(f"{stage}: over the {args.tool_seconds * 2}s time limit") from error
    if result.returncode != 0:
        detail = (result.stderr or b"").decode("utf-8", "replace")[-1600:]
        raise Refused(f"{stage} failed, exit {result.returncode}: {detail}")
    return result.stdout


def to_wasm(args, work, memory_bytes):
    wasm = work / "guest.wasm"
    source = Path(args.input)
    if source.suffix == ".wasm":
        shutil.copyfile(source, wasm)
    elif source.suffix == ".wat":
        run([args.wat2wasm, *locked_features(args.wat2wasm), str(source), "-o", str(wasm)], args, "wat2wasm")
    elif source.suffix in (".atto", ".ores", ".ll"):
        ir = source.read_bytes() if source.suffix == ".ll" else run(
            [args.compiler, "--target=wasm32", "--guest-abi=tenant-vm", str(source)], args, "compile")
        (work / "guest.ll").write_bytes(ir)
        actors, has_main = source_kinds(ir.decode("utf-8", "replace"))
        exports = [f"--export=oreslang_vm_actor_{i}_{name}" for i in range(actors) for name in ("init", "on_message")]
        if has_main:
            exports.append("--export=main")
        run([args.wasm_clang or args.clang, "--target=wasm32-unknown-unknown", "-Oz", "-nostdlib", "-x", "ir", "-mno-bulk-memory",
             "-mno-reference-types", "-mno-simd128", "-c", str(work / "guest.ll"), "-o", str(work / "guest.o")], args, "IR to wasm32")
        run([args.wasm_ld, "--no-entry", "--import-memory", "--gc-sections", f"--initial-memory={memory_bytes}",
             f"--max-memory={memory_bytes}", "-z", f"stack-size={memory_bytes // 2}", "--stack-first",
             "--allow-undefined-file=" + str(write_allowed_undefined(work)), *exports,
             str(work / "guest.o"), "-o", str(wasm)], args, "wasm-ld")
    else:
        raise Refused(f"unknown input type {source.suffix}")
    return wasm


def source_kinds(ir):
    """Actor types, in the order the compiler numbered them, and whether there is a main."""
    indexes = sorted(int(m) for m in re.findall(r"define i64 @oreslang_vm_actor_(\d+)_on_message\(", ir))
    if not indexes or indexes != list(range(len(indexes))):
        raise Refused("no tenant VM actors in this IR; was it compiled with --guest-abi=tenant-vm?")
    return len(indexes), re.search(r"define i64 @main\(\)", ir) is not None


def write_allowed_undefined(work):
    path = work / "host-imports.txt"
    path.write_text("".join(name + "\n" for name in HOST_IMPORTS))
    return path


def symbols(args, library):
    out = run([args.nm, "-g" if platform.system() == "Darwin" else "-D", "-u", str(library)], args, "nm").decode()
    return [line.split()[-1] for line in out.splitlines() if line.strip()]


def check_library(args, objects, library):
    """The build must contain exactly what the adapter says it does: no surprises from wasm2c."""
    bad = []
    for name in symbols(args, library):
        bare = name.split("@")[0]
        if platform.system() == "Darwin" and bare.startswith("_"):
            bare = bare[1:]
        if bare not in ALLOWED_IMPORTS:
            bad.append(bare)
    if bad:
        raise Refused("lowered code imports something it must not: " + ", ".join(sorted(set(bad))))
    for obj in objects:
        for bare in writable_symbols(args, obj):
            if bare not in ALLOWED_WRITABLE_SYMBOLS:
                raise Refused(f"writable global {bare} in lowered code: it would be shared between tenants")
        out = run([args.nm, str(obj)], args, "nm").decode()
        if re.search(r"\b(?:__tls_get_addr|_tlv_bootstrap)\b", out) or re.search(r"\s[Tt]LS|\.tbss|\.tdata", out):
            raise Refused("thread-local storage in lowered code: it can allocate during a turn")


def writable_symbols(args, obj):
    """Names in sections that stay writable. Function-pointer tables in position-independent code
    are in relocated read-only sections, which the symbol type alone cannot tell apart."""
    names = []
    if platform.system() == "Darwin":
        for line in run([args.nm, "-m", str(obj)], args, "nm").decode().splitlines():
            match = re.search(r"\(__DATA[^,]*,(__data|__bss|__common)\).*?\s_?(\S+)$", line)
            if match and not match.group(2).startswith(("l_", "L", ".L", "ltmp")):
                names.append(match.group(2))
        return names
    for line in run([args.objdump, "-t", str(obj)], args, "objdump").decode().splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[-3] in (".data", ".bss", ".tdata", ".tbss", "COMMON") or \
                (len(parts) >= 4 and parts[-3].startswith((".data.", ".bss.")) and not parts[-3].startswith(".data.rel.ro")):
            if parts[-1] != parts[-3]:  # the section's own symbol
                names.append(parts[-1])
    return names


def lower(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    source_size = Path(args.input).stat().st_size
    if source_size > 4 * MAX_MODULE_BYTES:
        raise Refused(f"input of {source_size} bytes is too large")
    memory_bytes = args.memory_bytes
    if memory_bytes % PAGE or not 0 < memory_bytes <= MAX_MEMORY_BYTES:
        raise Refused("memory must be a whole number of Wasm pages, within the limit")
    source_actor = Path(args.input).suffix in (".atto", ".ores", ".ll")
    with tempfile.TemporaryDirectory(prefix="tenant-vm-lower-") as temporary:
        work = Path(temporary)
        wasm = to_wasm(args, work, memory_bytes)
        data = wasm.read_bytes()
        actors, has_main = source_kinds((work / "guest.ll").read_text(errors="replace")) if source_actor else (0, False)
        required = [f"oreslang_vm_actor_{i}_{name}" for i in range(actors) for name in ("init", "on_message")]
        required += ["main"] if has_main else []
        required = required if source_actor else ["turn"]
        report = admit(data, required, memory_bytes)

        # Validation happens here too, with every feature the admission does not expect turned off.
        run([args.wasm2c, *locked_features(args.wasm2c), str(wasm), "-o", str(work / "guest.c"), "--module-name=guest"], args, "wasm2c")

        defines = [f"-DORES_VM_MEMORY_BYTES={memory_bytes}u", f"-DORES_VM_SCRATCH_BYTES={args.scratch_bytes}u",
                   f"-DORES_VM_TABLE_SLOTS={report['table_slots']}", "-DWASM_RT_USE_MMAP=1",
                   # No call-depth counter. It lives in a thread-local on a carrier shared by every
                   # tenant, and a kill skips the decrement, so one tenant could use up the depth of
                   # the next. The carrier's guard region is the limit instead. The macro changed
                   # name between wabt releases, and a build that misses both fails at link time.
                   "-DWASM_RT_USE_STACK_DEPTH_COUNT=0", "-DWASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1"]
        # Bounds checks keep actors of one tenant apart and give Wasm its specified traps. The cage
        # still has to hold if they fail. `guard` runs the same code with no checks, so that tests can
        # show the cage alone protects other tenants. It is a test tool, not a production choice.
        defines += ["-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1"] if args.mode == "bounds" else ["-DWASM_RT_MEMCHECK_GUARD_PAGES=1", "-DWASM_RT_SKIP_SIGNAL_RECOVERY=1"]
        if source_actor:
            header = (work / "guest.h").read_text()
            def generated(export):
                match = re.search(r"\b(w2c_guest_" + re.escape(export) + r")\(", header)
                if not match:
                    raise Refused(f"generated header does not declare the {export} export")
                return match.group(1)
            lines = [f"ORES_VM_ACTOR({i}, {generated(f'oreslang_vm_actor_{i}_init')}, {generated(f'oreslang_vm_actor_{i}_on_message')})"
                     for i in range(actors)]
            if has_main:
                lines.append(f"ORES_VM_MAIN({actors}, {generated('main')})")
            (work / "kinds.inc").write_text("\n".join(lines) + "\n")
            defines += ["-DORES_VM_SOURCE_ACTOR=1"]
        # NDEBUG: an assert in generated code would abort the process, with every tenant in it.
        flags = ["-std=c11", "-O2", "-DNDEBUG", "-fPIC", "-w", "-fvisibility=hidden", "-fwrapv", "-fno-strict-aliasing",
                 "-fno-delete-null-pointer-checks", "-fno-finite-loops", "-fno-stack-protector", "-fno-sanitize=all",
                 "-fno-builtin-malloc", f"-Wframe-larger-than={MAX_FRAME_BYTES}", "-Werror=frame-larger-than",
                 f"-I{args.include}", f"-I{work}", f"-I{args.wasm_rt_include}"]
        if platform.system() != "Darwin":  # Apple's linker and ABI probe large frames already
            flags.append("-fstack-clash-protection")
        flags += defines
        objects = []
        for name, source in (("guest", work / "guest.c"), ("adapter", Path(args.adapter))):
            obj = work / f"{name}.o"
            run([args.clang, *flags, "-c", str(source), "-o", str(obj)], args, f"lower {name}")
            objects.append(obj)
        suffix = ".dylib" if platform.system() == "Darwin" else ".so"
        library = out / f"guest{suffix}"
        link = ["-dynamiclib", "-Wl,-undefined,error"] if platform.system() == "Darwin" else \
               ["-shared", "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack", "-Wl,--no-undefined"]
        run([args.clang, *link, "-o", str(library), *map(str, objects)], args, "link")
        check_library(args, objects, library)
        digest = hashlib.sha256(library.read_bytes()).hexdigest()
        manifest = {
            "schema": "ores-tenant-vm-guest-v1",
            "library": library.name,
            "sha256": digest,
            "input": Path(args.input).name,
            "input_sha256": hashlib.sha256(Path(args.input).read_bytes()).hexdigest(),
            "wasm_sha256": hashlib.sha256(data).hexdigest(),
            "mode": args.mode,
            "memory_bytes": memory_bytes,
            "scratch_bytes": args.scratch_bytes,
            "admission": report,
            # Kinds are actor types in source order, then main if there is one.
            "kinds": {"actors": actors, "main": actors if has_main else None},
            "tools": {name: run([tool, "--version"], args, name).decode(errors="replace").splitlines()[0]
                      for name, tool in (("clang", args.clang), ("wasm2c", args.wasm2c))},
        }
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", help=".atto, .ores, .ll, .wat or .wasm")
    p.add_argument("--out", required=True)
    p.add_argument("--mode", choices=("bounds", "guard"), default="bounds",
                   help="guard is for tests of the cage alone, not for production")
    p.add_argument("--memory-bytes", type=int, default=131072)
    p.add_argument("--scratch-bytes", type=int, default=4096)
    p.add_argument("--include", required=True, help="directory holding oreslang/runtime/TenantVmGuestAbi.h")
    p.add_argument("--adapter", required=True)
    p.add_argument("--wasm-rt-include", required=True)
    p.add_argument("--compiler", default="oreslang-llvmc")
    p.add_argument("--clang", default="clang", help="lowers generated C for this host")
    p.add_argument("--wasm-clang", default="", help="lowers IR to wasm32, when --clang cannot")
    p.add_argument("--wasm-ld", default="wasm-ld")
    p.add_argument("--wat2wasm", default="wat2wasm")
    p.add_argument("--wasm2c", default="wasm2c")
    p.add_argument("--nm", default="nm")
    p.add_argument("--objdump", default="objdump")
    p.add_argument("--tool-seconds", type=int, default=120)
    p.add_argument("--tool-memory-bytes", type=int, default=4 << 30)
    args = p.parse_args()
    try:
        manifest = lower(args)
    except Refused as refusal:
        print(f"REFUSED: {refusal}", file=sys.stderr)
        return 2
    except (OSError, IndexError, ValueError, UnicodeError, MemoryError, RecursionError) as error:
        print(f"REFUSED: internal error treated as refusal: {error!r}", file=sys.stderr)  # fail closed
        return 3
    print(json.dumps({"sha256": manifest["sha256"], "library": manifest["library"], "mode": manifest["mode"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
