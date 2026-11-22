# Native LLVM Result<int,Error> parser ABI (host foundation)

HOST_ONLY C11 substrate: tenant-scoped Error identity is two integer fields;
the struct has no guest heap pointer, no cross-tenant reference and no
transferable capability. Strict signed i64 ASCII decimal, 1..1024 bytes,
negative accumulation, malformed/overflow to Err, Error code 1 (invalid
integer). Copies preserve original Err id. Fatal error ID exhaustion and
invalid host arguments are distinct status codes and not ordinary Err.

Neither the LLVM native AOT source emitter nor the copied Rust frontend
yet compiles the canonical Result/Option/int.parse/?/trap example. This host
ABI MUST NOT be reported as source guest parity. Implement typed HIR/MIR,
native tagged Result representation, Error object identity and matched
function cleanup paths, and verify exact SHA native guest execution.

Related: atto-spec #124; atto-platform-conformance #68; atto-compilers-
parity-testing #20. Current code has no tenant memory isolation proof.
