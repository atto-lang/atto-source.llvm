# Atto std/metrics, std/json, std/zorde — native-source milestone

This is **AttoLang (.atto) source** in each compiler's own repository.
The public import intent is `std/metrics`, `std/json`, and `std/zorde`,
subject to each compiler's bundled-library resolver. The serialization name
is **Zorde**, explicitly **not `std/serde`**.

These are independently maintained backend-owned implementations. Shared
behavioral conformance is desirable, but a shared byte-identical source file is
not a requirement.

## Example imports

```atto
import module metrics from "std/metrics";
import module json from "std/json";
import module zorde from "std/zorde";

pub routine main(): void {
  let counter = metrics.counter(100);
  counter.increment();

  let result = json.parse("{\"ok\":true}");
  if result.is_err(); do
    rt fail("invalid JSON");
  fi

  let mut Array<int> body = new Array<int>();
  body.add(1);
  let encoded = zorde.encode_frame(12, 1, body);
  if encoded.is_err(); do
    rt fail("invalid frame");
  fi
  return;
}
```

## `std/metrics` (in-memory only)

- `Counter`: nonnegative integer increments and a constructor-selected
  nonnegative maximum; failed updates are no-ops.
- `Gauge`: absolute `set` with inclusive lower/upper bounds.
- `Histogram`: up to 64 ordered nonnegative finite upper bounds, plus a
  final implicit +Infinity bucket. `observe` counts accepted nonnegative
  samples; `sum` and `count` are bounded at 2,147,483,647 and rejected
  observations increment a saturating diagnostic count.
- `valid_name` checks an ASCII metric name up to 128 Unicode scalars.
- Each mutable instance is owned by one actor/task and is **not** an atomic,
  shared, lock-free, distributed or concurrent registry. A collector actor may
  own instances and export detached snapshots later. No I/O, implicit logging
  or metric exporter is performed in the current module.

## `std/json` (strict document admission, **not** a full value tree)

- `decode(text)` / `parse(text)` produce `Result<Document, String>`
  after consuming an entire strict JSON document. `valid(text)` returns a
  bool. `encode(document)` / `stringify(document)` return original text,
  **not** canonicalized JSON; whitespace and numeric lexemes are preserved.
- Enforces JSON number grammar, strict string escapes, valid surrogate pairs,
  no unescaped control characters or lone surrogate codepoints, and exact
  full-input admission. Duplicate object keys are rejected after unescaping;
  Unicode normalization is not performed.
- Limits: at most 1,048,576 input Unicode scalars, 64 nested levels,
  262,144 JSON values, 128 keys per object, 256 decoded scalars per key and
  65,536 whole-parse key comparisons. Limits fail closed rather than
  silently truncating.
- `decode_int` admits strict integer-only JSON scalars across the signed
  64-bit range including `-9223372036854775808` and rejects overflow.
  `decode_i32` imposes the signed 32-bit range. Floating/exponent input is
  not silently rounded to an integer.
- The scalar snapshot of input is materialized before the input-scalar quota
  is checked; therefore this version is **not a streaming or
  pre-allocation-safe parser**. Bound input at the transport boundary.
- Full `JsonValue` allocation, typed `decode<T>`, streaming tokenization and
  recursive `stringify` must integrate with the existing **alloc/json**
  staged PR series; this module does not declare those complete.

## `std/zorde` (schema-bound bootstrap binary framing)

This module is **not a replacement** for the independent
`atto-serialization-and-validation` Zorde validators, TypeSpec/JSON Schema
contracts, CBOR/MessagePack/Protobuf codecs or WIT boundaries.

The current provisional envelope is:

```text
offset 0..3  : ASCII "ZORD" (0x5a 0x4f 0x52 0x44)
offset 4     : bootstrap frame version 0x01
then         : schema_id unsigned canonical varuint31 (positive)
then         : schema_version unsigned canonical varuint31 (positive)
then         : payload_length unsigned canonical varuint31
then         : exactly payload_length octets, no trailing data
```

The `ZORD` marker is a **backend-local experimental transport envelope**,
not a finalized ecosystem-wide Zorde wire standard. Schema payloads require
separately specified codecs.

- `Writer` has a 1..1,048,576 byte upper bound with atomic
  `write_byte`, `write_varuint`, `write_bytes` and
  `write_length_delimited`. `finish` detaches its result.
- `Reader` validates incoming octets, detaches them, reports truncated,
  overlong and overflowing varuints without advancing its cursor and
  preserves position for invalid length-delimited reads.
- `encode_frame`, `decode_frame`, `decode_frame_for` check quotas,
  exact length, canonical varuint, expected schema and version. Their
  invalid-input API reports `Err`, not host runtime fallback.
- `encode_bool_frame` / `decode_bool_frame`: exactly one 0/1 octet.
- `encode_i31_frame` / `decode_i31_frame`: signed
  `[-1,073,741,824, 1,073,741,823]` ZigZag31, canonical varuint.
  This is intentionally not a general signed i64 codec.
- No serialization of capabilities, actor references, pointer handles,
  closures, functions or arbitrary heap graphs. Generated/typed codec
  integration and migration policies require separate proof.

Reference wire octets:

```text
schema 7 v1 payload [0,255]:
  5a 4f 52 44 01 07 01 02 00 ff
schema 42 v2 bool true:
  5a 4f 52 44 01 2a 02 01 01
ZigZag31(-1073741824):
  ff ff ff ff 07
```

## Verification — evidence must remain separate

- Source-only: `node --test tests/std-metrics-json-zorde-source-contract.test.mjs`.
  The test checks source structure, interface safety bounds and independent
  reference varuint vectors. It **cannot** establish compiled Atto semantics.
- Java/GraalVM: `mvn -B -ntp -Dores.runtime.carriers=native
  -Dtest=NativeAttoMetricsJsonZordeTest -DfailIfNoTests=true test`.
  The focused GitHub workflow checks exact commit/tree and unskipped
  Surefire guest execution. A queued or zero-step workflow is NOT_EXECUTED.
- LLVM: the source-only workflow verifies the checked-in .atto files. LLVM
  native compiled guest execution, semantic checks and target AOT remain
  **NOT_VERIFIED** until separately executed.
- None of this establishes zero-GC behavior, production OpenTelemetry
  parity or complete cross-compiler serialization compatibility.
- Alternate private-carrier CI requires independently authorized source
  export and complete root Git tree + per-blob SHA validation; a carrier's
  cross-org GITHUB_TOKEN is not authorized to fetch private source.

## Follow-on gates

1. Exact-head Java guest execution; fix parser/typechecker/borrow diagnostics.
2. LLVM parser + native executable against the same positive/negative vectors.
3. Source-generated AOT serializers integrating the existing Zorde codecs.
4. Alloc-backed `JsonValue`, lossless numeric tokens and streaming decoder,
   without shadowing the active Java `alloc/json` PR series.
5. Bounded labeled metrics/registries, actor collector and async exporter.
