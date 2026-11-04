# LLVM native guest receipt smoke (partial)

The two AttoLang `.atto` programs each perform a real integer assertion,
decide their own outcome, print their own ordered `ORES_TEST` event and
summary, and return a matching OS exit. The host uses LLVM IR, Clang O0/O2,
`NativeStringRegion.c`, and a real stdout pipe. The verifier does not
generate any test events.

These two independently authored, one-case programs are **not** the full
`ores.std/testing` module or Java `TestCase`/`TestSuite` parity. Native
LLVM import of `ores.std/testing` must remain rejected until the runtime
owns correct generic counting, names, skip/filter/fail-fast, and nested suites.
A source-only verifier self-test proves formatting rejection, not guest
execution. Zero-step GitHub Actions jobs remain `NOT_EXECUTED`.
