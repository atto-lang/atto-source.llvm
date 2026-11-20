# Heterogeneous DOP geometry: four physical layouts

Status: **native C++ host/runtime-substrate only**, not an Oreslang source-lowering, guest execution or actor-heap completion claim. See the shared parity contract at `oreslang-platform-conformance` PR #145 and tracker #144.

`DopHeterogeneousLayout.hpp` complements the existing homogeneous `DopLayout.hpp`; it does not change or replace the old `Plan` / `Layout` ABI.

## Contract and layout cases

The shared mixed-width field order is:

| Field | Width / alignment | SoAoS group |
| --- | --- | --- |
| id (u32) | 4 / 4 | 0 |
| x (f64) | 8 / 8 | 1 |
| flag (bool) | 1 / 1 | 0 |
| next (ref32) | 4 / 4 | 1 |

* **AoS:** naturally aligned record, 24-byte stride. Row 1 `x` is byte 32 of segment 0.
* **SoA:** independent field-sized segments. Three rows contain 12 + 24 + 3 + 12 = **51 payload bytes**, but a single naturally aligned actor allocation requires **56 physical bytes**: segment base offsets are 0, 16, 40, 44. Row 2 `x` is byte 16 in segment 1, absolute actor byte 32.
* **AoSoA:** power-of-two tile width 4; five rows occupy 2 × 72 = 144 bytes including tail padding; row 4 `id` is byte 72.
* **SoAoS:** groups `[id,flag]` and `[x,next]` independently packed as arrays of subrecords, with 8- and 16-byte strides respectively; row 2 `next` is byte 40 of segment 1.

The `total_bytes()` accessor reports logical segment payload bytes (51 for SoA); `physical_bytes()` reports the **fully aligned allocation extent** (56 for SoA). `segments()[i].base` and `absolute_address(row,field)` resolve disjoint actor-relative physical positions without exposing native pointers. `max_total_bytes` compares against `physical_bytes()`, not the unpadded payload sum. This closes the discrepancy found during the shared conformance #145/#149 review.

Plans perform arithmetic in checked unsigned 64-bit byte units. Invalid field width/alignment, invalid group partition, illegal tile width, row/field bounds, overflows, oversized per-segment quota and aggregate actor quota must fail before the caller allocates or publishes any guest storage. A native plan is immutable by API; each caller must still authenticate actor/domain, generation, lifetime, access mode and borrow authority before resolving any address.

`tests/dop_heterogeneous_layout_test.cpp` cross-checks the exact golden offsets above, all four layouts with capacities 0/1/2/3/5/8/17, AoSoA tile widths 1/2/4/8, and exhaustively verifies natural alignment, in-segment bounds and bytewise disjointness of logical fields. It also exercises quota/overflow and malformed plans.

## Local execution evidence

The original and subsequent aligned-quota hardened header/test pair were independently built with GCC 14 and Clang 17 in C++17 mode, strict `-Wall -Wextra -Wpedantic -Werror -UNDEBUG`, and both test executables exited with status 0. Committed Git object blobs were checked against the locally compiled header/test. This proof is restricted to the two named host source files. It is **not** a successful `oreslang-llvmc` build, LLVM-generated guest code, native actor-memory ownership integration, or complete CTest/CI matrix.

## Next compiler gates

1. Type the source-level `u32`, `f64`, `bool` and `ref32` fields without collapsing all record fields into i64; reject unsupported guest constructs explicitly until lowering exists.
2. Lower source `define inverted class` and `LayoutStore<T>` into `HeterogeneousPlan` offsets inside the correct **actor-owned heap**, not process-global allocation or transient host object wrappers.
3. Emit a real Oreslang fixture: deterministic entity creation, update and lookup, and stable generation-checked row handles; execute emitted LLVM guest output. Compare equivalent GraalVM guest output and the shared oracle for AoS and SoA first, then AoSoA/SoAoS.
4. Run exact-head source CI with actual executed job steps, native sanitizers, and independent private-source export approvals when using a carrier in another organization.

ECS entities are rows/components; they are **not** each actors. A store may be actor-owned or task/region-owned, with identical layout geometry but separately enforced lifetime and ownership authority.
