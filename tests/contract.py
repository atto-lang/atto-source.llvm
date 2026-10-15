#!/usr/bin/env python3
"""CTest harness: verify safe rejection, verified IR, and optional native execution.

All checks use explicit control flow. Python removes bare 'assert' statements
under -O / PYTHONOPTIMIZE; a release harness must never silently skip gates.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile


class ContractFailure(Exception):
    """A required compiler, IR, linker, or executable property did not hold."""


def require(condition, detail):
    if not condition:
        raise ContractFailure(detail)


def ir_without_comments(source):
    """Remove LLVM IR comments without eating semicolons inside strings."""
    cleaned = []
    for line in source.splitlines():
        quoted = False
        escaped = False
        for pos, char in enumerate(line):
            if escaped:
                escaped = False
            elif quoted and char == "\\":
                escaped = True
            elif char == '"':
                quoted = not quoted
            elif char == ";" and not quoted:
                line = line[:pos]
                break
        cleaned.append(line)
    return "\n".join(cleaned)


def ir_function_definitions(source):
    """Select real LLVM function definitions, excluding globals and metadata.

    A --ir substring in @str = constant c"call i64 @foo" or a !dbg record
    must not impersonate executable lowering. Multiline headers, single-line
    definitions, quoted symbol names, and semicolons in IR strings are kept.
    Clang checks syntax separately when --verify-object or --run is used.
    """
    selected = []
    inside = False
    saw_open = False
    depth = 0
    for line in ir_without_comments(source).splitlines():
        if not inside:
            if not re.match(r"^[ \t]*define\b", line):
                continue
            inside = True
        selected.append(line)
        quoted = False
        escaped = False
        for char in line:
            if escaped:
                escaped = False
            elif quoted and char == "\\":
                escaped = True
            elif char == '"':
                quoted = not quoted
            elif not quoted and char == "{":
                depth += 1
                saw_open = True
            elif not quoted and char == "}":
                depth -= 1
        if saw_open and depth == 0:
            inside = False
            saw_open = False
    return "\n".join(selected)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--ir")
    parser.add_argument("--diagnostic")
    parser.add_argument("--run", type=int)
    parser.add_argument("--clang")
    parser.add_argument("--target")
    parser.add_argument("--native-runtime")
    parser.add_argument("--experimental-owned-int", action="store_true")
    parser.add_argument("--no-native-abi", action="store_true")
    parser.add_argument("--no-main-link", action="store_true")
    parser.add_argument("--verify-object", action="store_true")
    args = parser.parse_args()
    # A rejected-source test must never silently skip other requested gates.
    if args.diagnostic is not None:
        require(bool(args.diagnostic), "--diagnostic requires nonempty text")
        require(args.run is None and args.ir is None and
                not args.no_native_abi and not args.no_main_link and
                not args.verify_object and args.native_runtime is None,
                "--diagnostic cannot be combined with positive test modes")
    if args.ir is not None:
        require(bool(args.ir), "--ir requires nonempty text")
    if args.run is not None:
        require(0 <= args.run <= 255,
                "--run requires an OS exit code in [0, 255]")
    require(not (args.no_main_link and args.run is not None),
            "--no-main-link cannot request successful execution")
    require(args.native_runtime is None or args.run is not None,
            "--native-runtime requires --run")
    require(not args.verify_object or bool(args.clang),
            "--verify-object requires --clang")
    cmd = [args.compiler]
    if args.target:
        cmd.append("--target=" + args.target)
    if args.experimental_owned_int:
        cmd.append("--experimental-owned-int")
    cmd.append(args.fixture)
    compile_result = subprocess.run(
        cmd, capture_output=True, text=True, check=False, timeout=20
    )
    if args.diagnostic:
        require(compile_result.returncode == 1,
                f"invalid source must produce a compiler diagnostic (exit 1), got {compile_result.returncode}")
        require(args.diagnostic in compile_result.stderr, compile_result.stderr)
        require(not compile_result.stdout, "compiler emitted stdout after rejection")
        return
    require(compile_result.returncode == 0, compile_result.stderr)
    require(compile_result.stdout.startswith("; ModuleID"), compile_result.stdout)
    require(not compile_result.stderr, compile_result.stderr)
    if args.ir:
        require(args.ir in ir_function_definitions(compile_result.stdout),
                "expected executable LLVM IR was absent (comments do not count): " + args.ir)
    if args.no_native_abi:
        require("oreslang_owned_i64_region_" not in compile_result.stdout,
                "ordinary source unexpectedly references the experimental native heap ABI")
    # A textual IR substring is not proof of a valid compiler output. For
    # compile-only positive fixtures, assemble with real Clang and reject
    # invalid IR even when --ir or --no-native-abi otherwise matches.
    if args.verify_object:
        # Clang happily assembles a comment-only or declare-only module into
        # an object. A positive source fixture must emit at least one actual
        # function body; comments and forward declarations are not proof.
        executable_def = re.search(
            r"(?m)^[ \t]*define[ \t]+[^\n]*@",
            ir_without_comments(compile_result.stdout),
        )
        require(executable_def is not None,
                "positive LLVM IR contained no function definitions")
        with tempfile.TemporaryDirectory(prefix="oreslang-verify-object-") as directory:
            object_file = pathlib.Path(directory) / "guest.o"
            assembled = subprocess.run(
                [args.clang, "-x", "ir", "-", "-c", "-o", str(object_file)],
                input=compile_result.stdout, capture_output=True,
                text=True, check=False, timeout=30,
            )
            require(assembled.returncode == 0,
                    "positive LLVM IR failed object compilation: " + assembled.stderr)
            require(object_file.is_file() and object_file.stat().st_size > 0,
                    "Clang did not produce a nonempty native object")
    # Match a real IR *definition*, not a substring inside LLVM comments.
    # The Oreslang host wrapper must define a zero-argument i32 main.
    host_main = re.search(
        r"(?m)^[ \t]*define[ \t]+(?:[A-Za-z_][A-Za-z0-9_().]*[ \t]+)*"
        r"i32[ \t]+@main[ \t]*\([ \t]*\)",
        compile_result.stdout,
    )
    if args.no_main_link:
        require(args.run is None, "--no-main-link cannot request successful execution")
        require(bool(args.clang), "--no-main-link requires clang")
        require(host_main is None,
                "library-only source unexpectedly received a native OS entrypoint")
        with tempfile.TemporaryDirectory(prefix="oreslang-missing-main-") as directory:
            object_file = str(pathlib.Path(directory) / "guest.o")
            executable = str(pathlib.Path(directory) / "guest")
            # A failure to parse/compile bad LLVM IR is NOT proof that an
            # otherwise valid library lacks a process entry point.
            assembled = subprocess.run(
                [args.clang, "-x", "ir", "-", "-c", "-o", object_file],
                input=compile_result.stdout, capture_output=True,
                text=True, check=False, timeout=30,
            )
            require(assembled.returncode == 0,
                    "library-only LLVM IR failed object compilation: " + assembled.stderr)
            # Supplying a valid main must make the otherwise linkable library
            # succeed. Otherwise a diagnostic about a missing main can hide
            # *additional* unresolved library dependencies and pass falsely.
            # Assemble the stub independently so no extra source is injected
            # into the generated LLVM IR compilation.
            stub_object = str(pathlib.Path(directory) / "entrypoint.o")
            stub = subprocess.run(
                [args.clang, "-x", "c", "-", "-c", "-o", stub_object],
                input="int main(void) { return 0; }\n",
                capture_output=True, text=True, check=False, timeout=30,
            )
            require(stub.returncode == 0,
                    "main stub failed object compilation: " + stub.stderr)
            control = subprocess.run(
                [args.clang, object_file, stub_object, "-o", executable],
                capture_output=True, text=True, check=False, timeout=30,
            )
            require(control.returncode == 0,
                    "library failed positive link with main stub: " + control.stderr)
            link = subprocess.run(
                [args.clang, object_file, "-o", executable],
                capture_output=True, text=True, check=False, timeout=30,
            )
            require(link.returncode != 0,
                    "source with no main() linked as a successful standalone process")
            # A substring like "-lmain" or "__libc_start_main" must never
            # make an unrelated linker failure count as a missing entry point.
            # Match main as the actual missing symbol, never as a mention
            # on a subsequent line in an otherwise unrelated linker error.
            missing_main = (
                re.search(
                    r"(?im)\b(?:undefined reference to|undefined symbol:|unresolved external symbol)"
                    r"\s+[^A-Za-z0-9_\s]?_?main\b", link.stderr,
                )
                or re.search(
                    r'(?im)^undefined symbols? for architecture [^\n:]+:\s*\n\s*"?_main\b',
                    link.stderr,
                )
                or re.search(
                    r"(?im)\b(?:entry point|entrypoint)\s+[^A-Za-z0-9_\s]?_?main\b",
                    link.stderr,
                )
            )
            require(missing_main is not None,
                    "library link failed without a missing-main diagnostic: " + link.stderr)
        return
    if args.run is None:
        return
    require(bool(args.clang), "--run requires a clang executable")
    require(host_main is not None,
            "native executable lacks an i32 main entrypoint wrapper")
    with tempfile.TemporaryDirectory(prefix="oreslang-llvm-test-") as directory:
        executable = str(pathlib.Path(directory) / "guest")
        clang_cmd = [args.clang, "-x", "ir", "-"]
        if args.native_runtime:
            native_source = pathlib.Path(args.native_runtime)
            clang_cmd.extend(["-x", "c", str(native_source), "-std=c11",
                              "-I", str(native_source.parents[2])])
        clang_cmd.extend(["-o", executable])
        native = subprocess.run(
            clang_cmd, input=compile_result.stdout, capture_output=True,
            text=True, check=False, timeout=30
        )
        require(native.returncode == 0, native.stderr)
        result = subprocess.run(
            [executable], capture_output=True, text=True, check=False, timeout=10
        )
        require(result.returncode == args.run,
                f"native exit {result.returncode}, expected {args.run}: {result.stderr}")


if __name__ == "__main__":
    try:
        main()
    except (ContractFailure, OSError, subprocess.TimeoutExpired) as error:
        print(f"contract test failed: {error}", file=sys.stderr)
        sys.exit(1)
