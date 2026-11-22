# core/types v1 — LLVM native/AOT

This is this backend's independently maintained `core/types.atto` source, not an import of the Java, LLVM or common-source implementation.

**Scope:** signed-64-bit `int` scalar value width/min/max, logical bool/byte value widths, and signed/unsigned narrowing *admission predicates* over an existing signed `int`. This does not implement sized integer representations, narrow casts, native ABI layouts, runtime reflection, or compiler-minted type identity.

`void` remains a valueless return marker. A first-class `Unit` requires a proven singleton aggregate representation; compiler `TypeId` and `TypeInfo<T>` require closed-world metadata provenance, not host reflection or user-supplied type names. Unsupported features must diagnose, not silently fall back.

**Conformance fixtures:** `tests/fixtures/core-types-v1/edge-positive.atto` must import and link this exact source and execute as a compiled guest with exit 0; `void-value-negative.atto` must be rejected before artifact emission. Repeat across relevant native/AOT/wasm modes where supported. Report unsupported separately from failure.

**Status:** staged source and fixture only. No successful exact-head guest run or negative diagnostic has been established. No source tree exported to another org. This PR is draft until executed CI evidence and importer/linker behavior are verified.

**Independence:** preserve API/behavioral compatibility; implementation is backend-owned and can diverge physically.

## Namespace-only source layout

The canonical Atto `.atto` file uses the file-level `use namespace types;` declaration and public top-level functions, **not** `define module types`. Namespace is source identity, not a dependency import: consumers must use an explicit static import. This changes no runtime allocation or intrinsic guarantee. Parsing, linked guest execution and negative fixture rejection are distinct gates and remain NOT_EXECUTED until the compiler actually runs.

## Native LLVM explicit function imports

The current C++ frontend admits `import fnc ... from "relative.atto";`, but not the wildcard-import syntax in other backend fixtures. LLVM's positive and two semantic-negative guests therefore use explicit named imports and direct calls. This differs in fixture spelling without weakening the range, width, or negative-rejection contract; the 63-case matrix remains intact. The source header `use namespace types;` is admitted only for function-only file links. Unverified `import namespace`, package identity federation, actor classes and nominal type crossing remain denied. Actual executed native guests are still NOT_EXECUTED until runner steps exist.
