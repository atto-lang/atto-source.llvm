# Native class imports, strings and console output

The LLVM backend compiles the two-file `Foo` example in
`tests/fixtures/native-class-string/` to machine code. It uses the same source
as Java, including `import class`, `define pub class`, fieldless instance
methods, private `self` calls, typed `val` declarations, and the omitted
semicolon before the private method's closing brace.

From the repository root, after building `oreslang-llvmc`:

```sh
./scripts/compile-aot.sh tests/fixtures/native-class-string/main.ores /tmp/ores-foo
/tmp/ores-foo
```

Set `CLANG` if Clang is not on PATH. The driver emits LLVM IR and links the C11
runtime where needed. The executable needs no Java installation, interpreter,
JIT or tracing garbage collector. Its exit status is zero and stdout is exactly
`hello world\nhiiii 2hiiii`, without a final newline.

## Implementation and lifetime

Strings are immutable UTF-8 sequences with explicit lengths, including embedded
NULs. Checked scalar types keep string handles distinct from guest integers.
Literal bytes remain static; concatenations own native buffers. A thread-local
root invocation region owns the descriptor table and concatenation buffers.
Its OS entry wrapper creates the region before guest execution, drains console
output after guest execution, and then deterministically frees the region.
There is no tracing, reference counting or runtime evaluation of source code.

The current limits are 65,536 descriptors and a conservative 64 MiB cumulative
string-byte budget, counting literal and concatenation lengths. Exhaustion
latches failure; the wrapper returns status 1 after cleanup. These are bounded
implementation limits, not the final language-wide memory policy. The current
allocator retains concatenation buffers until root exit; it is not yet a
long-running server string allocator.

Leading console effects and local declarations lower in source order using
compiler-owned anonymous slots. This preserves effects in initializers rather
than hoisting declarations above prints. Import visibility, private instance
methods, receiver borrowing and fieldless object liveness remain checked.

## Multi-argument console parity (bounded native source slice)

The native compiler admits 1–32 String arguments in `stdio.print` and
`stdio.println`. It evaluates each expression once, left to right,
concatenates with no inserted separator, and emits one console operation
(one final newline only for `println`). It uses checked String-Add AST
lowering and the existing invocation-owned String region, not libc `printf`.
Mixed scalar types and 33+ arguments fail before emitting LLVM IR.

The Rust CTest class/String runner contains imported Foo source, expression
side-effect ordering, embedded NUL and UTF-8, the exact 32-argument boundary,
and type/arity rejections. Both LLVM and Java executable source runs must
pass on pinned revisions before cross-backend parity can be certified.
This does not imply general native stdlib, async or Spin support.

## Nonblocking console transport

Guest print calls enqueue immutable views in a bounded 1,024-record SPSC queue.
One same-process output reactor writes using `O_NONBLOCK` and readiness polling.
Guest code does not wait for stdout readiness or create one thread per print.
The OS wrapper joins the reactor before destroying any referenced string bytes.
Queue exhaustion, output errors or a five-second readiness stall fail the run.
Shutdown rechecks the queue after observing completion so a final print cannot
be lost to a stale empty-queue observation.

Regular-file stdout is rejected at startup because it cannot guarantee strict
nonblocking writes. Use a terminal or pipe (for example `/tmp/ores-foo | cat`).
There is no blocking file-worker fallback. The runtime restores stdout's file
status flags before returning from the root invocation. This initial transport
assumes exclusive ownership of stdout configuration during that invocation.

## Verification and remaining parity work

The Rust `native_class_string_parity` runner executes native programs and
compares exact output and status with Java when `--java-command` is supplied.
CTest registers release and ASan/UBSan variants. Cases cover the original
program, effect ordering, function arguments and returns, lazy expressions,
Unicode and NULs. Negative cases require rejection before any IR is emitted.
A C runtime test checks repeated region destruction, invalid handles, descriptor
and byte limits, failure recovery and unsupported regular-file output.

This milestone does **not** establish full Spin parity. Strings currently require
a synchronous host `main`; actor, suspension and exception-frame integration
are rejected until their ownership and cleanup boundaries are lowered. String
fields, collections, mixed-type concatenation, string equality, unpaired UTF-16
surrogates and the full standard-library API remain parity gaps. General local
declarations after arbitrary control-flow statements remain unsupported.
Allocation and console I/O are visible to effect analysis and are not silently
admitted as allocation-free hard realtime work.
