# Actor-owned tagged wire v1 — shared Java/GraalVM and LLVM contract

**Status:** host/runtime codec substrate. Source-level tagged `send` lowering,
derived `Serializable`, general exhaustive `match`, typed actor replies, and
arbitrary `Future<T>` continuation lowering are **not** implemented by this
change. Both implementations must fail closed on unsupported guest forms.

## Required actor semantics

- `a.send(value)` is the only approved guest actor method call for messages.
  Actor methods are not externally callable, including `receive`.
- At compile time the sender expression must be `Serializable` recursively
  and belong to exactly one alternative of the receiver's closed message
  domain. Ambiguous/overlapping types or aliases are rejected, not resolved by
  declaration order. A pattern is a type-only union; an enum is a nominal
  type with case values and may occur inside a pattern, not vice versa.
- The compiler will eventually derive and pin nonzero, stable numeric
  `schema_id` (u64, whole closed contract) and `variant_id` (u32, exact
  message alternative). Duplicate/colliding IDs are a hard build error.
  These IDs are **not** cryptographic authenticators and alone do not prove
  source typing. Never accept arbitrary guest-supplied IDs as type evidence.
- Enqueue moves ownership of the *serialized bytes* to the scheduler or
  creates a detached copy. No mutable object pointer, class, callable,
  borrow, ActorRef, native handle, or capability may cross actor boundaries.
  The receiver authenticates its declared schema and exact variant before
  dispatch or decoding. No direct sender-to-receiver callback.
- Shared actors use this same binary default. Possible future optimized
  `rt copy` must separately prove equal isolation and ownership semantics.

## Byte format (network byte order)

Header length **24**, no padding:

| Offset | Length | Description |
| --- | --- | --- |
| 0 | 4 | ASCII `OMSG` |
| 4 | 1 | version `01` |
| 5 | 1 | flags `00`, all other values rejected |
| 6 | 2 | reserved `0000`, nonzero rejected |
| 8 | 8 | nonzero schema ID, unsigned big endian |
| 16 | 4 | nonzero variant ID, unsigned big endian |
| 20 | 4 | payload size, unsigned big endian |
| 24 | variable | exactly one tagged value |

A tagged value recursively encodes:

| Tag | Value |
| --- | --- |
| `01` | bool + one canonical `00` or `01` |
| `02` | signed 64-bit integer in two's-complement, big endian |
| `03` | u32 byte length + strict, shortest-form UTF-8 |
| `04` | positional tuple: u32 arity + N encoded values |
| `05` | array: u32 count + N encoded values |
| `06` | list: u32 count + N encoded values |

The nested wire representation does **not** itself encode type variables or
nominal enum identifiers; the compiler's exact per-variant contract does that.
For example a `List<int>` and `List<string>` cannot be exchanged merely
because they both carry tag `06`. A decoder must not interpret a matching
numeric ID as a substitute for that compiler proof.

Reject trailing bytes, truncated fields, unknown tags, invalid UTF-8,
noncanonical booleans, oversized or ambiguous payloads, unsupported version,
bad contract/variant, and all reserved flags. Limits: frame <= 1 MiB, nodes
<= 4096, nesting depth <= 64. Successful native unpack allocates an owned
frame; it **must** be released on delivery, failure, cancellation and actor
finalization. Java decoding returns a detached immutable value tree.

## Cross-backend golden vector

The same fixture (schema `0102030405060708`, variant `11223344`) is
tuple `("hi", 7, Array[true,false], List["ok"])`. Exactly 66 bytes:

```text
4f4d5347010000000102030405060708112233440000002a
040000000403000000026869020000000000000007
050000000201010100060000000103000000026f6b
```

Java `ActorMessageWireV1Test` and native
`native_actor_owned_tagged_wire_v1` assert exact bytes plus negative
contract, malformed, UTF-8 and container cases. Until a funded workflow
executes those suites at their exact commits, tests added are not test passes.

## Next compiler and scheduler gates

1. Derive the *same* canonical schema fingerprint and stable variant keys on
   both compilers; lower each legal `send` into a typed owned envelope and
   validate receiving shape before a receiver turn starts. Atomic mailbox
   admission must transfer ownership exactly once or return it on rejection.
2. Exhaustive `match` checks closed alternative coverage, duplicate arms,
   overlapping structural shapes, tuple destructuring and narrowed bindings.
   A wildcard may cover a residual open type but may not conceal a missing
   variant of a closed contract.
3. Extend `Future<T>` and `await` using a generation-fenced, one-shot
   completion `VALUE | FAILED | CANCELLED`. A suspended actor releases its
   logical lane; completion enqueues CONTROL, and the continuation resumes on
   a fresh admitted actor turn. Value ownership is transferred once.
4. On actor stop, panic, cancellation and forced retirement, cancel or settle
   pending replies exactly once, close outstanding state/FFI loans safely,
   run permitted teardown, and free all retained wire buffers after no
   continuation can still dereference them.

None of these steps authorizes direct `a.someMethod()` calls or bypasses
the actor receiver's declared message contract.
