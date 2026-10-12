# Core library source

`assert.ores` mirrors the Java repository's source-owned `std/assert` module.
See [assertion API and backend status](../docs/ASSERT.md).

This source is staged for future native stdlib linking. The LLVM compiler currently
rejects imports, Any/bool assertion signatures and ordinary failure/trap effects;
merely placing this module here does not make Assert callable in native programs.
