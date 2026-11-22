# LLVM-native Atto HTTP stdlib message-policy slice

`stdlib/http/message_policy.atto` is a **separate compiler implementation**,
not an identical copy or a dependency on the Java/GraalVM `std/http`.

Available: scalar HTTP status classes; no-body statuses/HEAD/CONNECT rules;
request TE+CL conflict admission; nonnegative header/body byte budgets; and
non-overflowing partial-frame boundaries. Source-only, freestanding and without
ambient runtime authorization.

LLVM's current native source-import linking is not established for this module.
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
