# Bounded exhaustive invocation loans

`tests/native_borrow_alias_matrix.rs` enumerates every alias partition for calls
with two or three borrowed arguments, every shared/exclusive mode assignment,
and three owner types: `int`, `bool`, and a nominal scalar class. The oracle
rejects exactly when two arguments name the same owner and either is exclusive.
This produces 144 cases: 60 valid programs and 84 required rejections.

Every valid program reads every argument, executes two successive calls and
places the second call inside a lazy conditional expression. This checks both
value preservation and release of call-scoped loans. Every invalid native program
must exit with the specific loan-conflict diagnostic and emit no LLVM IR.
Valid native programs are linked and executed; a successful linker must actually
produce a fresh executable. Empty compiler output and fake link success are
explicitly rejected by local harness calibration.

The CTest registrations run all cases with normal linking and ASan/UBSan, with
sanitizer recovery disabled. Missing Rust/Clang tooling produces a failing test,
not omitted coverage. The host test runner uses only the Rust standard library;
it does not change guest allocation, add GC, or become part of native artifacts.

The optional Java command checks the identical generated programs. Valid cases
must execute with result 42; invalid cases must reach the borrow-conflict checker,
not fail during parsing or for an unrelated type error. For example:

```sh
build/oreslang-native-borrow-alias-matrix \
  --compiler build/oreslang-llvmc --clang clang \
  --java-command /absolute/path/to/java-launcher.sh
```

Local validation: all 144 cases agree between native and pinned Java
`79e88c627efa9522a0990077301eb6611dd9320c`; all 144 also pass under native
ASan/UBSan. This is exhaustive only for these bounded alias/mode combinations.
Escaping loans, projected-place disjointness, suspension, concurrency and FFI
provenance require their own contracts; this matrix does not prove the entire
borrow checker correct. The shared Java/native TSV corpus includes these 144 vectors alongside its
18 existing nested-reborrow and receiver-lifetime cases (162 total).
The Rust generator can reproduce its matrix section with `--emit-corpus PATH`.
