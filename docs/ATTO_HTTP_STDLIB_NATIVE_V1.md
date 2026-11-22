# LLVM-native Atto HTTP stdlib message-policy slice

`stdlib/http/message_policy.atto` is a **separate compiler implementation**,
not an identical copy or a dependency on the Java/GraalVM `std/http`.

Available: scalar HTTP status classes; no-body statuses/HEAD/CONNECT rules;
request TE+CL conflict admission; nonnegative header/body byte budgets; and
non-overflowing partial-frame boundaries. Source-only, freestanding and without
ambient runtime authorization.

LLVM now stages a bounded **compiler-owned `std.http` wildcard namespace import slice** for scalar public free functions only.
The canary concatenates the **actual .atto stdlib source** and a 46-case
`main` into one native guest compilation unit, avoiding false claims of import
support. `bash ci/test_http_stdlib_policy.sh` builds the native compiler,
compiles the guest into an ELF executable and checks exit 42.

This is *not* a socket, HTTP/1.1 parser, TLS, QUIC, HTTP/3, actor HTTP server,
or streaming client implementation. Future modules must validate field-name
casing, duplicate Content-Length, obs-fold, message-body delimiters, trailer
syntax, percent escapes, and network capabilities in the actual byte parser.

Overlaps reviewed: Java #495 is separate `.ores` net protocol migration;
common-source #14 supplies QUIC/HTTP3 policy; LLVM #549 is an unrelated
native HTTP exchange lifetime contract. No in-flight files are modified.

Status: native guest CI not executed yet. A stepful exact-head runner result
is mandatory before declaring executable parity.

## Independent numeric/framing parity hardening — 2026-11-22

Added two LLVM-native `.atto` functions, not copied from another backend:

- `http_request_framing_fields_allowed(content_length_fields,
  transfer_encoding_fields, final_chunked, unsupported_transfer_coding)`:
  explicitly deny duplicate framing fields, TE+CL, non-final chunked and
  unsupported transfer codings. All counts must come from the original
  validated HTTP wire-header set, not normalized or deduplicated values.
- `http_body_chunk_allowed(consumed, next_bytes, max_bytes)`:
  subtraction-first overflow-safe chunk admission with a 1GiB ceiling.

The scalar native guest canary now has **46** distinct return-code checks
(34 prior plus 12 adversarial boundary cases) and a single success exit 42.
The script always rebuilds the exact checked-out LLVM compiler and verifies
the authored case count before accepting exit 42. No import-namespace linker
is claimed here; the candidate source and canary are still concatenated
as one unit. Native guest CI remains **NOT_EXECUTED** pending stepful runner
admission; source-only parity is not compiled parity.


## Real compiler-owned namespace import slice (2026-11-22)

The LLVM native AOT entrypoint now recognizes this exact import shape:


 `import * as http from "std.http";`

The registered backend-owned source is embedded at compiler build time from
`stdlib/http/message_policy.atto`. It is lexed and parsed as a **separate native
Atto AST**, public `pub fnc` declarations are enumerated at file scope, and
qualified calls (for example `http.status_valid(200)`) are bound to the matching
compiler-owned function (internally `http_status_valid`) **before** the
normal typed checker and LLVM IR emitter. This is not filesystem import
resolution and is not concatenating library and test source.

Unregistered namespaces, missing/duplicate import directives, private or
unknown members, direct unqualified builtin symbols, namespace alias
shadowing, freestanding import attempts, and unsupported `import module`
syntax reject before native code emission. The dedicated
`tests/http_stdlib/namespace_import_canary.atto` has **17** compiled-guest checks;
`ci/test_http_namespace_import.sh` includes **nine** negative cases and an
ambient shadow-file canary.

**Limits:** this is not a generic import graph and does not yet implement
`define pub module`, nested public modules, imported classes, cross-unit
cycles, alias import of non-function values, or any network operation. Those
features must continue to fail explicitly. `--freestanding-core` still rejects
hosted HTTP imports. The prior 46-case same-unit HTTP policy canary is
retained as a separate test to distinguish scalar policy from linked imports.

The previous CI script contained a malformed `grep` expression and duplicated
execution footer; it has been replaced and now creates an offline root
Cargo lockfile, runs Rust compiler unit tests, builds the exact checked-out
compiler and executes all 46 source-policy canaries. The separate namespace
workflow step compiles/executes the real importer guest.

All these cases are **AUTHORED / NOT_EXECUTED** until an exact-head Actions
job has actual executed steps and passing guest receipts. See
atto-platform-conformance#101 for 11-backend conformance tracking.

## Core import borrow and Copy-only ABI gate (2026-11-22)

The compiler-owned `std.http` module linker now invokes
`validate_http_core_copy_boundary` on the **independently parsed Atto AST**
before it can enter the typed compiler. Every imported function must accept
only `int` or `bool` arguments and return `int` or `bool`; generator,
class, borrowed/non-Copy, heap and other stateful signatures reject with
`E_NATIVE_IMPORT_BORROW`. A Rust test mutates the parsed library AST to
try non-Copy returns, non-scalar parameters, generator effects and class
declarations. All are authored but await actual Rust execution.

The browser/LLVM frontend already rejects `self` as an ordinary owned value.
Its existing class move checker rejects a use after transfer. Three additional
compiler-front-end test cases are staged in
`frontend/tests/core_ownership_boundary.rs` and corresponding `.atto`
fixtures to prove moved-owner denial, transferred-owner access and Copy scalar
reuse. The same-unit HTTP policy script now executes this frontend test before
the native policy guest.

`ores.core.collections` contains generic APIs whose `Copy` bounds cannot
yet be proven by this scalar compiler. Its source is **not** admitted into
the native import registry; the ninth negative import case verifies
`E_NATIVE_IMPORT_UNREGISTERED` rather than silently importing an unverified
borrow/ownership API.

This is **scalar Copy ABI enforcement only**, not a general borrow checker
for imported generic collections, suspension lifetimes or actors. The
11-backend core ownership obligations are in atto-platform-conformance#101
and atto-compilers-parity-testing#19; all real guest receipts remain
`NOT_EXECUTED`.
