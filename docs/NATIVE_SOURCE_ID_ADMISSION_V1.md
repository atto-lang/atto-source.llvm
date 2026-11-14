# Native source-ID admission v1 (LLVM backend)

**Status:** host-only metadata parsing and static-table admission. **NOT** native `.atto` compiler lowering, instrumented MIR, runtime spans, LLVM ORC JIT or five-backend tracing parity.

This LLVM/C++ implementation is intentionally independent of Java/Truffle's AST and the Rust/Cranelift compilers. It validates the versioned common observable ID format while keeping backend-specific descriptor tables and codegen policy.

## Supported now

- Parse checked-in `rid_` / `sid_` lowercase 128-bit hexadecimal identifiers into fixed 16-byte native values.
- Reject wrong-kind IDs, uppercase/malformed/zero IDs without partially modifying the output parameter.
- Validate an already-resolved, bounded static site descriptor table with unique local indices and site IDs.
- Keep parsing and admission in a cold path; do not allocate/format ID strings on an ordinary log hot path.
- A standalone C++17 test can compile without LLVM libraries.

## Exact host-only test command

~~~sh
cmake -S . -B build-source-ids -DATTO_SOURCE_IDS_HOST_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-source-ids --parallel
ctest --test-dir build-source-ids --output-on-failure
~~~

Independently, the header and test compiled and executed in the authoring environment using Clang C++17 `-O1 -fsanitize=address,undefined` and GCC C++17 `-O2` with `-Wall -Wextra -Wpedantic -Werror -UNDEBUG`; each printed `LLVM_SOURCE_ID_HOST_METADATA_PASS`. This is a native **host-only** test, not a compiler guest execution proof.

## Needed before implementation promotion

1. Link canonical package-relative source units, macros, overloads and checked-in source registry to LLVM frontend's **own typed MIR**.
2. Emit fixed descriptor indices and metadata into the actual compiled native object; prove source-map sidecar is paired with the deployed executable/build ID.
3. Independently insert native AOT probes into LLVM IR, verify all exceptional/cancellation/unwind exits and retained logical identities after optimization/inlining.
4. Implement authenticated `none/boundaries/functions` policies and OFF→ON→OFF→ON for *precompiled* probes. Disable all unapproved patching in hard real-time/strict sealed AOT.
5. Verify separately any real LLVM ORC compiler-produced variants; host function-pointer switching or a dormant-probe toggle does **not** prove JIT compilation.
6. Reuse OTel semantic event formats and actor/continuation trace context from common contracts, but do not import Graal, .NET, Forge or Rust compiler code.

References: [source identity specification #62](https://github.com/ores-truffle-oreslang/attolang-language-spec.md/pull/62), [common native instrumentation #279](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/279).
