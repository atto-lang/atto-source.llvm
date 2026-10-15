# Sealed/final/permits native source admission

**Status: fail-closed only.** The Java reference compiler has a draft source
admission implementation (Java PR #1023). LLVM **does not** yet implement this
source-language feature. Native ownership/heap helpers, the existence of native
class allocation, or compiler acceptance of an unknown identifier are not
evidence of correct sealing.

Planned source contract:

```oreslang
define sealed class WebEvent permits Click, Scroll as
end

define final class Click from WebEvent as
end

define final class Scroll extends WebEvent as
end
```

- `sealed`, `final`, and `permits` are contextual declaration words.
- Permits must enumerate **direct** children, or be inferred only from the
  complete same-compilation-unit hierarchy; otherwise reject.
- Direct children must remain closed via `sealed` or `final`.
- Cross-module and imported hierarchies need verified complete link metadata.
- `final` classes cannot be subclassed, `sealed` cannot combine with `final`.
- Closed pattern matching cannot claim exhaustiveness until the concrete
  hierarchy metadata drives verified coverage analysis.
- Native class allocation must preserve ownership/borrows, deterministic
  drops and actor isolation; sealing does not imply shareability.
- Actor class hierarchies and `non_sealed` remain outside the first milestone.

`tests/native_sealed_permits_fail_closed.py` and its CTest registration
reject all representative sealed/final/permits source forms with nonzero status
and **no emitted LLVM IR**, until both grammar and semantic analysis exist.

### Exit criteria

1. Native parser/AST captures the complete header, including unresolved
   permitted nominal IDs and generation-safe module identities.
2. Link-time checker validates exact allowed direct subtypes across imports,
   with stale/hot-reload metadata rejected.
3. Native lowering enforces final inheritance and closed dispatch.
4. Match exhaustiveness and failure diagnostics agree with Java.
5. Positive and negative shared-source fixtures run on both backends with
   exact Git-head and tree verification before claiming parity.
