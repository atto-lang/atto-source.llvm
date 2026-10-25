# Native compile-time module linking

The host compiler now statically resolves relative module selections before
name/arity/effect/ownership checking and LLVM emission:

```ores
import module math from "./math";
pub fnc main(): int { return math.add(40); }
```

```ores
// math.ores
// The selected module name must match; an explicit `as` changes the local binding.
define module math as
  fnc delta(): int { return 2; }
  pub fnc add(int n): int { return n + delta(); }
end
```

Compile with the normal `oreslang-llvmc` or `scripts/compile-aot.sh`. The output
contains the reachable admitted function bodies. Removing the source files or
removing Java/Clang/the Ores compiler from PATH does not affect execution.
This is source-to-native compilation, not runtime import interpretation.

The linker parses each unit separately, resolves module-qualified calls against
that unit's selected public function/arity slots, and qualifies implementation
symbols before whole-program validation. Dependency-private functions get internal
LLVM linkage. Identically named helpers/classes in different units retain distinct
identities. Private class storage and synchronous scalar/class borrows inside a
module retain the existing native ownership proof; cross-module scalar reborrows
are checked after binding, with lexical argument evaluation order unchanged.

Imports are canonical code-unit references, not textual include/replacement.
Repeated/diamond dependencies and same-file hard links are deduplicated; direct
self-imports, including symlink/hard-link aliases, fail. Declaration-only cycles
are admitted after graph collection. Observable `init` hooks are rejected until
once-only initialization ordering is lowered. Each requested module selection
must exist in its source file. Imported binding shadowing is explicitly unsupported
in this native slice and fails instead of changing call resolution.

Admission limits: 128 source files, 8 MiB aggregate source/read budget, 4,096
import edges and 32 recursive resolver edges. Files are opened nonblocking and
checked as regular files on the opened descriptor; FIFOs/devices are rejected.
UTF-8, NUL and target/capability directive checks cover dependencies as well as
the root. Relative `./`/`../` `.ores` paths (extension optional) are admitted.

The host linker also admits canonical `+/` project-root imports through the
nearest `.ores.pkg.toml`. This is intentionally a narrow fail-closed bootstrap,
not a general TOML/package resolver: the manifest must contain exactly one
top-level canonical `schema_version = "1"` marker; quoted/repeated/conflicting
schema keys, TOML multiline strings, `project.root`, missing/foreign manifest
symlinks, traversal/empty path segments, nested-package crossings and physical
symlink escapes are rejected. The selected source must remain below the physical
nearest package root. `pkg:`, bare/std/host resolution, runtime loading,
imported actors, module continuations and module initializer hooks remain
unsupported and fail closed. No partial LLVM IR is emitted when any dependency
is unsupported.

This does not yet compile Spin: exported class/method surfaces, String/Array
operations, typed HTTP capabilities, async continuations and OTel imports remain.
The native HTTP worker now imports and calls `http_status.ores`; its state stays
in the actor heap, and public/private helper calls execute as native machine code
inside the admitted mailbox turn. HTTP formatting/routing are still adapter work.

`native_source_module_graph_execution` covers executable graphs, private/arity
checks, conflicting loans, physical aliases, unsupported sources/targets and
bounded admission. Positive binaries run with all source files hidden and an
empty toolchain PATH. Twelve shared-source programs additionally execute against
Java with the primitive-loan-copy fix in Java PR #1380. Native named value arguments
and explicit type arguments on imported generic calls are tested separately and
are **not claimed as Java parity** (those current Java qualified-call forms fail).
The same imported generic function is compared through inferred generic arguments.

On macOS 26, the installed LLVM 20 ASan runtime can deadlock during its own
shadow-memory initialization, before guest main. A process sample confirmed that
failure; Clang 23 passed the affected sanitizer tests. Use a compatible sanitizer
toolchain (for example `-DORE_CLANG=/opt/homebrew/opt/llvm@23/bin/clang`) rather
than treating the initialization hang as an Oreslang runtime result.
