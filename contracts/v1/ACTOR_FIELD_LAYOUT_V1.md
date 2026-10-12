# Oreslang actor field layout v1 — shared LLVM / Graal contract

Status: experimental interoperable **i64 serial actor field fragment**.
This is NOT a promise of a shared Java object header or a packed native
memory layout: an implementation stores each owned field in its private
allocation domain. Only the public *logical field descriptors* are shared.

## Typed logical descriptor

- ABI version: `1`; source form: `define actor ActorName as ... end`.
- Actor name and field identifiers are ASCII `[A-Za-z_][A-Za-z0-9_]*`.
- Field count: 1..4096, in **source declaration order**. Index is zero-based.
- Each field has a UTF-8 name, stable index, type tag `i64` and
  `mutable` flag. Source `let` means true; `const` and `val` false.
- Duplicated names, invalid types, zero fields, too many fields, and
  write attempts to readonly fields must be rejected **before** execution.
- The descriptor grants no actor admission or heap authority. Native
  pointers, actor incarnation and turn epochs remain scheduler-private.

## Deterministic versioned fingerprint

Use FNV-1a 64-bit, offset `0xcbf29ce484222325`, prime
`0x100000001b3`, unsigned arithmetic wrapping modulo 2^64.

Feed bytes exactly in this sequence:

1. ASCII `ores.actor.fields.v1`, then a NUL byte.
2. Actor identifier UTF-8 ASCII bytes, then NUL.
3. For every field **in declaration order**:
   field name UTF-8 ASCII bytes and NUL; ASCII `i64` and NUL;
   exactly one binary byte `0x01` (mutable) or `0x00` (readonly).

No JSON whitespace, platform endianness, host pointer, reference identity,
process salt, or physical allocator address participates in this hash.

Sample descriptor:
```ores
define actor Counter as
  let count: int = 5;
  let offset: int = 7;
  fnc add(int by): int {
    self.count = self.count + by;
    return self.count + self.offset;
  }
end
```
Its hash is `0xb2d57409773a17ed`. For `Readonly` with one
`val offset: int` field the hash is `0xabaa8c384f278d29`.

Generated LLVM host-only symbols `ores_actor_NAME_layout_version()` and
`ores_actor_NAME_layout_hash()` return the contract version and fingerprint.
The native ActorGroup adapter must reject a mismatch **before adoption**.
The JVM reference helper `NativeActorFieldLayoutV1` validates and hashes the
same logical descriptor. The hash is a compatibility check, NOT a
cryptographic MAC or a security capability.

## Lifetime and isolation

One actor incarnation owns one native heap and one field table. Only one
logical turn accesses mutable fields at a time. `await`/FFI loans are opaque
pins: they may outlive a turn, cannot read while suspended, and prevent drop.
On stop/panic/cancel, prohibit new source work and enqueue parent CONTROL;
wait for loans and callback holders, admit cleanup as an exclusive actor turn,
drop the record in reverse field order, physically destroy the heap, then
acknowledge `get_done()`. Neither physical carrier identity nor completion
callback authority can replace the actor scheduler's admission contract.

## Out of scope

Other integers/floats, strings, structs/classes, references, variable-length
arrays, nested graphs, borrowed projections, cross-process representation,
source `spawn`/await lowering and automatic compiler exceptions/unwind are
future ABI revisions. A v1 hash match does not certify these behaviors or
full Java/Graal/LLVM language parity.
