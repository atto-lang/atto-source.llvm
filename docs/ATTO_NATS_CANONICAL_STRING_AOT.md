# AttoNATS canonical pretokenized String matcher — native LLVM AOT qualification

## Scope and provenance

This test directly targets the native AttoNATS implementation, not the
earlier integer-token-ID stand-in or a host-language matcher. Both original
application files are copied into this repository **without changing bytes**:

- `tests/fixtures/atto-nats/pretokenized_subjects.atto`: Git blob
  `1363e61ae08a1f960157345c33010cc0ebfc7068`
- `tests/fixtures/atto-nats/pretokenized_nats_match.atto`: Git blob
  `3923f6c2f7d72c652cec0b76c6bef3b04c37ee09`

Original application: [AttoNATS PR #21](https://github.com/atto-poc/atto-nats-clone.atto/pull/21),
source commit `36aea5983ebd86a0f6d86870e9eff771467e1242`.
Java/GraalVM independently compiled/executed these bytes via
[private run #43239287468](https://github.com/voxletra/voxletra-e2e/actions/runs/43239287468).

The native LLVM compiler currently rejects executable `use namespace`
headers. The gate checks the unchanged Git blobs and removes **only**
each file's first `use namespace` directive to compose one temporary
executable Atto body. The compiler must run the remaining body unchanged;
this is not a claim that verbatim multi-file namespaces execute on LLVM.

## Required compiler-backed execution

The CTest `native_atto_nats_pretokenized_string_aot` executes
`scripts/test-atto-nats-string-aot.sh` using the actual native
`oreslang-llvmc`, the Rust no-GC IR auditor and real Clang at O0/O2.

It requires:

1. Exact original source Git blobs and namespace header identities.
2. Nonempty native LLVM IR defining `i32 @main()`, with actual generated
   calls to `ores_string_equal` and `oreslang_int_array_get`.
3. Native no-GC IR auditor succeeds (which alone does **not** prove
   no allocations or a hard-real-time profile).
4. Two compile-negative Atto mutations reject with nonempty compiler
   diagnostics and **no partial LLVM IR**: integer inserted in String array,
   and integer substituted for owning class constructor argument.
5. Clang O0 **and** O2 native binaries execute with exit status 0,
   stdout exactly `ATTO_NATS_TOKEN_MATCH_PASS\n`, stderr empty.
   The native stdout reactor sees a **pipe**, never a regular file.
6. Compiled guest execution cannot be skipped, mocked, marked pending,
   or inferred from an infrastructure-failed zero-step workflow.

Run after building the compiler and auditor:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2 --target oreslang-llvmc oreslang-no-gc-ir-audit-build
ctest --test-dir build -R '^native_atto_nats_pretokenized_string_aot$' --output-on-failure
```

## Existing evidence and compiler compatibility

- **PASS**: Java/GraalVM executed original canonical String-array
  matcher and test guest at exact Atto blobs (link above).
- **PASS**: bounded *numeric* token-ID Atto guest compiled and executed
  with LLVM 18 native Clang O0/O2 via
  [run #43242456278](https://github.com/voxletra/voxletra-e2e/actions/runs/43242456278).
  This is not the same String-array source and not current LLVM main.
- **BLOCKED / precise historical compatibility evidence**:
  [older private LLVM run #43282977028](https://github.com/honeypot-r-us-test/hnpt-lib-core/actions/runs/43282977028)
  built the real compiler using previously provisioned private source
  but rejected canonical `if ... then` with `expected '{'` at byte 628.
  That historical compiler accepts only `if ...; then` or braces;
  **current LLVM source** already includes `accept_keyword_if_header()`
  and admits the no-semicolon canonical form. Do not rewrite the Atto
  application to preserve a deprecated backend syntax restriction.
- **NOT VERIFIED**: execution of this new full String-array CTest on
  current LLVM main. Source organization Actions have recent
  **zero-step runner failures**; those are not compiler test results.

## Next implementation blocker: sealed state and network bytes

The kernel uses already-validated, immutable subject tokens; it is not
an authenticated subscription index. Future native LLVM must lower
actor-owned `state{}` and authenticated generation-fenced critical
read/write loans, preserve state-borrow invalidation before suspension,
and perform genuine async socket Bytes operations without translating
routing logic into Rust/C++/Java.

Only an authenticated two-client TCP CONNECT/SUB/binary PUB/MSG/UNSUB test
on both toolchains, followed by 1/8/64/1024-subscriber throughput,
p50/p95/p99 latency, CPU, memory and allocation measurements, can
establish meaningful AttoNATS broker performance or parity.

**Review requirement:** Keep this as a draft until an exact-head
nonzero-step current LLVM test and independent review pass. No new
private LLVM source was exported to an unapproved external carrier.

### Historical native String-array kernel executed after explicit syntax adaptation

**PASS (historical compiler, NOT canonical source identity)**:
[private run #43284475095](https://github.com/honeypot-r-us-test/hnpt-lib-core/actions/runs/43284475095),
job `131414085353`, original existing private LLVM compiler tree unchanged
at base carrier `622be2cca084187e856f1706847429d2d3693f65`.
The carrier branch head
`c2761fb80dd1a17408bc4b83319f01760eb52010`
added only the two byte-identical Atto app blobs and a test workflow,
never new LLVM private compiler source.

The workflow built the actual LLVM 18.1.3 C++ compiler, then constructed
a **derived diagnostic source file** by (1) removing only the two
`use namespace` headers and (2) replacing trailing
`if <condition> then` with the older compiler's equivalent
`if <condition>; then`. Its logs explicitly state
`CANONICAL_ATTO_NATS_SOURCE_EXECUTED=false` and
`HISTORICAL_IF_THEN_HEADER_ADAPTATION=DIAGNOSTIC_ONLY`.

The historical compiler emitted LLVM IR including the native String
equality and Array get lowering, and the actual AttoNATS matching
guest ran with Clang O0 **PASS** and O2 **PASS**. Both returned zero
with exactly the guest-origin `ATTO_NATS_TOKEN_MATCH_PASS`
stdout marker. This is meaningful historical backend algorithm
execution evidence, **not** canonical exact-source parity, not
proof for current LLVM `main` and not an actor/NATS broker test.

The committed CTest from this PR must still execute with
*only namespace header removal*, **no conditional syntax rewrite**,
using the current LLVM source before LLVM String-array parity is promoted.
