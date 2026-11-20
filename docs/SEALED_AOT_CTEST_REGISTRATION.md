# LLVM sealed-AOT fail-closed CTest entrypoint

Use `bash scripts/verify-sealed-aot.sh build` after configuring and building the native LLVM compiler.

The entrypoint runs the three sealed AOT CTests **individually**, using `--no-tests=error`. All three suites are always registered even if Clang is unavailable. Missing toolchains must fail a test rather than leave a no-test green status.

Positive tests should emit actual native LLVM IR, link via Clang, run the native guest with its expected result, and log an artifact digest. A failed or missing source-organization runner is **NOT_EXECUTED**, not a pass.

No assertion here establishes process zero-GC, general class dispatch or actor heap reclamation.
