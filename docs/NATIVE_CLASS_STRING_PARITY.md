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
NULs. This follows the shared `oreslang-common-source/docs/STRING_ABI_V1.md`
contract: ordinary String extent is never discovered with `strlen`.
`NativeCString` is a separate explicit FFI adapter whose payload length
excludes its trailing NUL and whose conversion rejects embedded NUL instead of
truncating the source value. Checked scalar types keep string handles distinct from guest integers.
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

## Console argument parity

The native host compiler accepts multiple `string` arguments in
`stdio.print(a, b, ...)` and `stdio.println(a, b, ...)` (up to 32
arguments). It evaluates each expression exactly once, left to right,
concatenates without implicit separators, and emits one console operation.
`println` appends one newline; `print` does not.

The compiler lowers this through its **existing typed String addition and
region-owned string runtime** rather than a second parser or a direct libc
`printf` escape hatch. Mixed `string`/`int` calls currently fail
closed until the common stringification contract is implemented.

The native class/String parity runner includes the cross-file `Foo` program
with `stdio.println("x", x)`, side-effect-order tests, and invalid mixed-type
rejections. This is a source-to-native regression; an unexecuted CI job does
not constitute verification.

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
fields, equality, typed arrays, and nested owning results have subsequent
native implementations; see [sum values and current lifetime limits](NATIVE_SCALAR_SUMS.md).
Mixed-type concatenation, unpaired UTF-16 surrogates and the full
standard-library API remain parity gaps. [Ordered locals](NATIVE_ORDERED_LOCALS.md)
admit scalar/sum slots, immutable records and scoped array owners after
statements. Mutable class owners and continuation-frame integration remain
unsupported.
Allocation and console I/O are visible to effect analysis and are not silently
admitted as allocation-free hard realtime work.
