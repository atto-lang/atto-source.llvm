# Oreslang generic conformance — stage 1

This contract defines explicit positive and negative source fixtures for native scalar generic resolution. Each positive source returns process exit code 42 after native LLVM IR compilation, Clang linking and guest execution; each diagnostic fixture must fail before IR with the expected diagnostic substring.

The native LLVM frontend currently supports only signed i64 generic specialization. **The contract does not claim LLVM supports arbitrary class, structural, borrow or actor generic specialization.** Non-scalar generics must fail closed. The fixture corpus is intended for eventual JVM/Java Native Image cross-backend comparison, but those backends must be executed before claiming parity.

Run the existing `tests/native_scalar_generic_source_execution.py --compiler ... --clang ...` to execute both its historical matrix and this contract. GitHub CI source mirrors must remain private, byte-verified and free of cross-org checkout credentials.
