# LLVM native allocation parity slice

Two shared-contract cases are mapped to the existing **actual compiled
`Box<int>` native guest** scripts: `lexical.normal_return_once` and
`lexical.early_return_once`.

The production C++ LLVM compiler emits LLVM IR; the test checks each
relevant program contains actual region allocation/read/drop/close call
sites, compiles and links it with clang and the real owned-i64 region,
and executes the resulting guest process expecting exit **42**.
The region's `close` operation aborts if allocations remain live;
a second drop of a retired handle also aborts. Compiler-negative
semantics additionally require an explicit diagnostic and no emitted IR.

`alloc-parity-llvm-v1.sh` runs the existing strict native oracle,
checks its completion markers, verifies the two guest fixtures are
identical to immutable Git HEAD blobs, and prints only **two**
`ATTO_ALLOC_CASE` records. Existing CI also runs the sanitizer
variant. Do not label this two-case subset as 22-case parity, general
generic `Box<T>` lowering, actor cancellation or automatic class drop.

The actual CI job remains **NOT_EXECUTED** until a runner performs the
tests on this exact SHA. Backend-owned `.atto` allocator library code
stays separate from shared conformance metadata.
