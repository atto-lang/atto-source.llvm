# Evidence record: experimental i64 inverted struct source adapter, 2026-10-30

## Scope and provenance

- Repository: `ores-truffle-oreslang/oreslang-source.llvm`, draft [PR #1213](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1213)
- Exact Git blob: `src/main/cpp/oreslang/frontend/ExperimentalInvertedI64.hpp` `43fb26ec69974d8148b9d4142d738f17bbbefa14`
- Source fixtures copied byte-for-byte into local temporary directory and checked against Git **blob** SHA (not just filename):
  - `positive-column-get.ores`: `a17849d07ac3974923e8dd575348d3452f9a51b0`
  - `positive-column-bounded.ores`: `b30cd7ee3a72d8c7dcf7d8a862684a36c5f2ee0b`
  - `negative-out-of-bounds.ores`: `4adbae1b55a02756d1f0a2eb2fc24f21aa19f12f`

The private source files were retrieved via the authorized GitHub connector into the assistant's ephemeral local build environment, **not** mirrored, published or transferred to another GitHub organization. A separately compiled local C++ driver consisted only of a `#include` of the **identical verified header blob** and a call to `experimental_inverted_i64::compile(source)` on the supplied source bytes. This tests the exact new adapter but **does not build or test `src/main/cpp/oreslang/main.cpp`, `oreslang-llvmc`, the overall LLVM compiler/linker, or CMake integration**.

## Executed local checks

Environment: Debian GCC 14.2.0, Swift LLVM Clang/Clang++ 17.0.0, hosted x86-64 Linux.

| Probe | Actual |
| --- | --- |
| GCC C++20 `-Wall -Wextra -Werror -pedantic -O2` header driver | PASS, compiled |
| Clang++ C++20 `-Wall -Wextra -Werror -pedantic -O2` header driver | PASS, compiled |
| Positive 1: exact .ores -> header source compiler -> textual LLVM IR -> Clang native -> process | PASS, exit `42` |
| Positive 2: exact .ores -> header source compiler -> textual LLVM IR -> Clang native -> process | PASS, exit `42` |
| Negative runtime: exact .ores -> compiler -> Clang native -> out-of-bounds read | PASS, `llvm.trap`, exit `132` (illegal instruction on tested x86-64) |
| 18 unsupported .ores source-negative cases (same programs as checked-in Python regression) | PASS, 18/18 specific expected rejections |
| GCC `-fsanitize=address,undefined -fno-omit-frame-pointer` header driver on both positive source programs | PASS, no reported ASan/UBSan errors |
| Clang++ driver -> Clang generated LLVM IR -> both positive native guest binaries | PASS, exit `42` each |
| Clang 17 -O1 with ASan/UBSan instrumentation of both generated LLVM-IR guest binaries | PASS, both exit `42`, no sanitizer reports |

These are **actual locally executed exact-header and exact-fixture native checks**, materially stronger than model-only tests. They are still *not* the complete production native LLVM compiler, source-org workflow, user-facing CLI, whole-project ABI, borrow checker, general Java/LLVM parity, wasm32 or actor-hard-RT certification.

## Remaining merge and release gates

1. Build the complete `oreslang-llvmc` target with production LLVM development libraries and confirm the exact CLI `--experimental-inverted-i64` dispatch, including normal-mode rejection and flag conflicts. No full-tree CMake build was executed locally.
2. Execute the checked-in `native_inverted_i64_guest` and `native_inverted_source_failclosed` CTest cases on a stepful authorized runner; reproduce native exact-head GCC/Clang, sanitizer and compiler IR evidence.
3. Replace the experimental stack-column source parser with shared HIR/MIR type/ownership lowering before enabling general source. The experimental slice has no checked generational row handles, `erase_swap`, nested packed struct, borrow escape checks or actor-domain owners.
4. Restore protected source/main independent review and approved private-CI destinations before any authorized cross-org source transfer. A zero-step Actions failure remains **NOT_EXECUTED**, never an implementation failure or pass.

**Verdict:** `LOCAL_EXACT_HEADER_AND_GUEST_FIXTURE_EXECUTED`; `FULL_LLVM_COMPILER_NOT_EXECUTED`; `FIVE_BACKEND_GUEST_PARITY_NOT_EXECUTED`.
