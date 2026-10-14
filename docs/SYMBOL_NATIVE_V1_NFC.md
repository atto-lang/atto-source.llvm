# Native Symbol v1 Unicode NFC admission

**Status: host-only conformance foundation, not LLVM guest-runtime or actor transport integration.**

The tagged-key codec lives at `src/main/cpp/oreslang/runtime/SymbolKeyCodecV1.hpp`. Its key envelope is `4F 4B 01 <kind> <u16-big-endian byte length> <UTF-8 payload>`.

## Unicode data dependency

- **Build without ICU:** ordinary String keys accept valid UTF-8, but PROCESS/STABLE Symbols accept only printable ASCII. Non-ASCII Symbols fail closed. No best-effort normalization or locale-dependent transformations.
- **Build with approved ICU:** the standalone CTest target enables `ORES_SYMBOL_V1_ICU_NFC` only with **ICU 76.x** (Unicode 16.0 tables). Runtime `u_getUnicodeVersion` must return exactly 16.0, otherwise non-ASCII Symbol admission fails closed.
- The codec checks strict UTF-8, preallocated bounded UTF-16 conversion (at most 256 Symbol-name UTF-8 bytes), canonical NFC **without normalizing incoming bytes**, C0/C1 controls, Unicode FORMAT characters and bidi control points.
- Native byte strings retain nominal identity: ordinary String `"ready"` and PROCESS Symbol `#ready` are distinct. NFC comparison is only for Symbol names. STABLE requires a canonical lowercase UUID.
- The native codec does **not** own an interning table and must never allocate unbounded process-global Symbol state on untrusted ingress.

## Manual host-only checks

From a clone with a C++17 compiler:

```sh
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -Isrc/main/cpp tests/symbol_key_codec_v1_test.cpp \
  -o /tmp/ores-symbol-v1-ascii
/tmp/ores-symbol-v1-ascii

# ICU 76.x / Unicode 16.0 only:
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -DORES_SYMBOL_V1_ICU_NFC=1 \
  -Isrc/main/cpp tests/symbol_key_codec_v1_test.cpp \
  $(pkg-config --cflags --libs icu-uc) \
  -o /tmp/ores-symbol-v1-icu
/tmp/ores-symbol-v1-icu
```

The full CMake CTest target is `native_symbol_key_codec_v1_host_only`. It is a host implementation test, **not proof** that the LLVM Oreslang frontend parses, typechecks or lowers Symbol/Map guest programs.

## Required before any guest-facing integration

1. Verify Java JDK Unicode data/normalizer behavior against the pinned 16.0 tables (including decomposed/composed equivalents and classification changes). A numeric version match alone does not prove parity.
2. Build a common v1 golden-vector file for Java and native: UTF-8 lengths, NFC rejection, prohibited format/bidi points, stable UUIDs and nominal namespace distinctions.
3. Integrate source-level Symbol values/typed Map and actor transport only after verifying authenticated tenant/endpoint/protocol-scoped exports. The default native untrusted allowlist is **not** an export manifest.
4. Exercise the full LLVM CMake/CTest, ASan/UBSan, Java JUnit and Native Image suites on the exact PR heads; link settings must not silently enable Unicode on unsupported ICU tables.
5. Keep Java #903, LLVM #380 and common-source #22 in draft until real source-level CI and parity succeed.

Last audited: October 14, 2026.
