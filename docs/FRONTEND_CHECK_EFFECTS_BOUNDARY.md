# Native LLVM frontend-only check vs effects analysis

The native compiler exposes `oreslang-llvmc --check file.atto` as a
**frontend-only** source admission path. It reuses real source validation,
import resolution, and supported-subset parsing, then returns before LLVM
IR generation, runtime setup, or executable output. It does not certify
complete semantic type checking, backend lowering, or execution.

`oreslang-llvmc --analyze file.atto` deliberately returns status 2 and
an explicit unsupported-capability diagnostic. The native frontend currently
does not implement typed `throws`, `raises`, `panics` diagnostics or
transitive effect inference. It must never return success just because a file
can be parsed. Until a shared typed compiler contract exists, use
`atto test --effects --file file.atto` for advisory lexical diagnostics.

The formatter's `oresfmt --analyze` delegates `--analyze` to the selected
compiler through `ORESLANG_BIN`. Pointing it at this native compiler will
therefore fail closed rather than fabricate missing effect information.
This is the proper current LLVM boundary, not Java/GraalVM effect parity.

## Verification

Build the native compiler and run:

```sh
sh tests/native_frontend_check_contract.sh ./build/oreslang-llvmc ./tests/fixtures
```

The script executes a valid `.atto` fixture, malformed source, unsupported
effects mode, mutually exclusive flags, and source-byte preservation.
It is a proposed test and is not itself proof of a green CI job.

Before claiming effects parity with Java, implement compiler-owned may-effect
resolution with explicit unknown results, no-codegen receipts tied to source
identity, typed direct/transitive call fixtures, and exact-head executable
tests across both compiler frontends.
