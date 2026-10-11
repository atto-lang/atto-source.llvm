# Experimental source-level LLVM String execution

Invoke \`oreslang-llvmc --experimental-string <file.ores>\`. This **opt-in**
bounded compiler mode generates verified LLVM IR, which must be linked to the
native C String runtime, e.g. with \`clang -x ir program.ll -x c NativeString.c
-I src/main/cpp -o program\`.

Currently accepted subset:

\`\`\`oreslang
pub fnc main(): int {
  val first: String = "ping";
  val duplicated: String = rt copy first;
  val moved: String = rt take duplicated;
  return len(first) + len(moved);
}
\`\`\`

* Each \`val\` holds one native, owned String value. \`rt copy\` uses a checked
  deep copy. \`rt take\` transfers ownership and zeros the old slot.
* Compiler rejects duplicate names, use after move, unsupported syntax,
  recursive/async calls, and implicit copying. String values are dropped
  in reverse scope order on normal return and runtime failure.
* The runtime allocates explicit buffers with **no GC**. Literal UTF-8
  validity, allocation/size errors, and cleanup are checked. UTF-8 byte
  lengths are returned from \`len\`.
* This is only a zero-parameter integer-returning entry function with
  local String values. It **does not** yet implement String parameters,
  String returns, RPC transports, JSON structures, concurrency/suspension,
  TJSV adapters, general ownership paths, or GraalVM parity.
* Error exit code 93 is diagnostic-only for this experiment, not the final
  language exception/Result/error semantics.

Real native fixture test: \`experimental_native_string_source_ownership\`
runs compiled IR linked against NativeString.c under sanitizers, along with
compile-time rejection and negative-UTF-8/error-cleanup cases.

Do **not** admit a production SDK or publish based on this experiment.
