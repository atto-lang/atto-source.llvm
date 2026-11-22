# Compiler-owned AttoLang libraries (LLVM)

The units listed in [`LIBRARIES.txt`](LIBRARIES.txt) are embedded into
`oreslang-llvmc` at configure time (`cmake/AttoLibraryEmbed.cmake`) and are
imported with the canonical selectors from
[atto-spec core-alloc-std-libraries-v1](https://github.com/atto-lang/atto-spec/blob/main/language/core-alloc-std-libraries-v1.md):

```atto
import module num from "core/num";
import module arena from "alloc/arena";
import module path from "std/path";
```

| Layer   | Units                                   | Requires |
| ------- | --------------------------------------- | -------- |
| `core`  | `num`, `cmp`, `option`, `result`, `mem` (+ legacy `numeric`) | nothing (freestanding) |
| `alloc` | `arena`, `vec` (+ legacy `int_buffer`, `int_buffer_bundle`) | allocator provider |
| `std`   | `io`, `path` (POSIX and Windows variants) | hosted target |

* `core/`, `alloc/` and `std/` never fall through to files or packages; unknown
  units are compile errors. `atto.<layer>/` and `.atto` suffixes are aliases;
  `ores.core/` and `ores.std/` are legacy aliases.
* Layering is enforced: core units may import only core; alloc units core and
  alloc. `std/*` is rejected on freestanding targets.
* `std/path` picks its POSIX or Windows source from the target OS (host by
  default, or `--target-os=linux|macos|freebsd|posix|windows`).
* Units are written in the subset this backend lowers natively today (i64,
  bool, string concatenation, `Option<int>`/`Result<int,int>` with methods,
  `loop`, local `Array<int>`). The public API, not the source, is the
  cross-backend parity boundary.

Tests: `tests/atto_libraries_v1.py` compiles, links and runs every
`tests/atto_libraries/*.atto` guest natively (ctest `native_atto_libraries_v1`).

`assert.ores`, `net/` and the `*_policy.atto` files are source candidates that
are not registered in `LIBRARIES.txt` and are not importable yet.
