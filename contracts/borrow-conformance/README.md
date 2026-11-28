# Borrow conformance L1 (native OwnershipCfg engine)

`tests/borrow_conformance_l1.sh` feeds the canonical ownership-graph corpora of
[atto-borrow-checker.rs](https://github.com/atto-lang/atto-borrow-checker.rs)
(`docs/CONFORMANCE_PLAN.md` there, level L1) straight into
`src/main/cpp/oreslang/types/OwnershipCfg.hpp`. Every verdict must either
conform (exact canonical code) or be listed with a reason in the corpus's
`*.llvm-known.tsv`. Unsupported ops are coverage gaps; they are counted, never
passed.

- `graph-cases-v1.json` and `graph-cases-hybrid-v1.json` are vendored byte-exact
  and pinned in `PIN` (source commit + git blob SHAs). Re-vendor all of them
  together; never edit them here.
- `build/borrow-conformance-l1/*.llvm.jsonl` are `atto-borrow-graph-results-v1`
  lines for `atto-borrow-conform graph-results <corpus> <results>`.

**First run (corpora at borrow-checker `f8ecc96`):**

| Corpus | Conform | Known differences | Unsupported |
|---|---|---|---|
| V0, 59 cases | 14 | 3 | 42 |
| Hybrid, 18 cases | 7 | 5 | 6 |

The known differences:

- **The engine decides beyond V0.** C18 and C19 are V0 join/cycle limits; the
  hybrid agrees with the engine on them.
- **Per-path rather than global owner-definition uniqueness** (C16).
- **A conservative loan join.** Any join with differing loan provenance is
  rejected as `E_CFG_JOIN_UNPROVEN` (H04, H05, H06, H13). The verdict
  matches canonical; the code is coarser.
- **SSA-like places.** No re-initialization after `Drop` (H15), so the
  engine is stricter than canonical for loop-local owners.

The unsupported cases are ops OwnershipCfg has no counterpart for: `Write`,
`WriteThrough`, `Copy`, `DeepCopy`, freeze, pin, weak, task and actor ops.
