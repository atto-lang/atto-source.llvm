# Experimental static i64 native FFI AOT proof (M0)

**DRAFT, NOT RELEASED.** This is a narrowly scoped, source-compiled native C ABI downcall, not a production ffi actor implementation. It is enabled only by BOTH --experimental-static-ffi-i64-aot and --ffi-grant=atto.math.i64.v1 with --target=host.

The intentionally closed-world test source declares a NativeMath ffi actor with exactly one extern suspends fnc add_i64(int a, int b): int and a suspends main body that awaits NativeMath.add_i64(20,22). The compiler emits a real LLVM i64(i64,i64) call to the immutable, approved native symbol atto_add_i64. The independent C shim is compiled and statically linked by Clang. No source-selected shared library, symbol alias, native pointer, varargs, callback, borrow escape or dynamic loading can be admitted.

**Important semantic limitation:** The prototype does not actually spawn a foreign actor or perform a scheduler suspension. Its static NativeMath.add_i64 invocation is TEST-ONLY SHAPE, NOT approved public AttoLang syntax or proof of actor RPC. Before production use it must be replaced with verified supervisor-granted actor admission, authenticated foreign artifact binding, cancellation, lifetime leases and checked Result/Error outcomes. The CLI grant is a local opt-in flag, NOT authenticated production permission. Never merge this as general ffi actor functionality.

Source arguments are deliberately bounded decimal 0..255, with sum no greater than 255 to prevent silent process-exit truncation in this test. Native ABI arguments and result remain signed i64. Unknown syntax, mismatched declarations, unsupported type, symbol, target, absence of capability, or other feature flags must reject before ANY IR is emitted.

## Real executable gate

Run: python3 tests/static_ffi_i64_native_e2e.py --compiler=/path/to/oreslang-llvmc --clang=/path/to/clang-18

This builds real native binaries from compiler-emitted IR and the separate approved C symbol, executes them, repeats with ASan/UBSan, demands a failed link without the approved shim, and tests malformed source and missing grants with zero stdout. Unsupported areas: arbitrary i64 values, signed overflow handling, actors/supervisors, callback scheduling, hot code loading, pointer escape, real JNI/.NET ABI, and Java/.NET/Forge parity. Do not claim any backend is supported without exact-head compiled-guest execution and reviewed sanitizer evidence.

Related drafts: atto-spec #54, atto-platform-conformance #49, atto-source.rs #244.
