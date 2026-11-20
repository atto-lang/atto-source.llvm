# Borrow admission audit — 2026-11-10

Base: `0ee037ca3e5dd9ac6ac1e9cc3d40f19ee6326a9c`.

The admission harness now includes three canonical `.atto` fixtures shared
byte-for-byte with the Java audit: immutable owner rejection, overlapping
loan rejection, and valid mutable owner admission. Rejection checks require
the specific semantic diagnostic and no emitted IR or created artifacts.
Summary counts are derived from the actual case arrays instead of stale
hard-coded text.

Local executed evidence:

- Native admission: **33** canonical denials, **6** canonical IR controls,
  and one explicitly legacy `.ores` IR control.
- Harness self-tests: **30** passed, zero failures/skips.
- Task boundary admission: **8** borrowed Task forms rejected before IR.
- Native alias matrix: **60** admitted cases executed with result 42,
  **84** conflicting cases rejected; ASan/UBSan enabled with Clang 23.
- New mutable-owner fixture: result 42 at **O0 and O2**, each with and
  without ASan/UBSan. Java's actual hosted guest also returns 42.

Fixture Git blobs:

| Fixture | Git blob |
| --- | --- |
| audit-immutable-owner.atto | `debcef23bf39b07d3f5bcf3a1b8f0aeaa7607b0b` |
| audit-mutable-owner.atto | `c56b9e63174ff2fbb762e71273c032c6473f490a` |
| audit-overlap.atto | `18cdecd7bac1a7e2ede50dee0260f381ceae760e` |

Compiler binary SHA-256:
`0761945ba51ed85e3896f522f6401fbabc993fcd2b7e353a9641c9a7b86a5f1f`.
Build with LLVM 20 (`LLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm`).
LLVM 23 could not build this baseline because it removed APIs used by the
backend; Clang 23 can still link and sanitize the emitted LLVM 20 IR.
LLVM 20's ASan runtime stalled during initialization on this macOS host;
the successful sanitizer results above use Clang 23.

Reproduce the admission tests by compiling the Rust harnesses directly with
`rustc --edition=2021`, then pass the real compiler path. For the alias matrix,
use `--compiler PATH --clang CLANG_PATH --sanitize`. Build the harness's
self-tests with `rustc --edition=2021 --test tests/native_borrow_admission.rs`.

These are bounded admission/native-execution results. They do not certify
all borrow semantics, all compiler adapters, Wasm guest loans, or trusted live
runtime race detection. Hosted Actions results are separate from local proof.
