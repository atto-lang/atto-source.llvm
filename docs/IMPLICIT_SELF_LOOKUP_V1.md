# LLVM native implicit-self call lookup v1 (draft)

Status: source implementation candidate; native compiler, Clang linkage, sanitizer and Java differential results are **NOT EXECUTED** on this PR head.

## Normative resolution order

Only a direct unqualified call in a genuine lexical instance-method body is eligible.

1. Live lexical parameters and local declarations take priority. Native scalar method-lowering currently does not implement local callable values, so a shadowed method call is rejected; never fall through to a method or free function.
2. The **lexical receiver class's method family** is selected by name before caller-visible arity. Only then select its exact name+arity slot. Wrong arity must fail even if a free function of that name and arity exists.
3. A same-named free function must not override a selected receiver method family; direct `read()` is semantically `self.read()`. An explicit/free qualified call is required to select the free function.
4. No method family: use the existing ordinary free function lookup. Do not use the receiver's dynamic class to discover subtype-only method names.
5. Reuse the explicit member-call lowering for private-member checks and compiler-owned `Borrow`/`BorrowMut` loans. No bound-method value, dynamic string lookup, actor-state spoofing or method capability is created.
6. Before any native method receives compiler-owned receiver authority, validate a **bidirectional** exact class declaration ⇄ lowered function link: nominal owner, qualified member name, value arity, public/private visibility, receiver mutability, unique lowered body and presence of every declared slot. A matching variable named `self` is not sufficient evidence of authorized method ownership.

Structured `ScopedLocal` declarations become visible **after** their initializer and stay live for subsequent statements **within the same enclosing block**. Each conditional/dispatch arm has an independently snapshotted owner, array and shadow environment; no declaration survives into sibling arms or the parent continuation. Index/foreach binders are visible only inside their loop condition/step/body and are revoked after the loop. A scalar binder must hide any previously nominal class/array binding with the same name. Ordinary function-level parameters and locals remain reserved for their function.

## Executable regression contracts

`tests/native_class_methods.py` registers and checks:
- `private-helper`, `arity`, `mut-receiver` native guest outputs;
- `family-over-free`: method wins over an exact-arity free function (exit code 42);
- `wrong-arity-no-free-fallback`: method family exists but arity is wrong (compile error, no IR);
- `scoped-shadow-does-not-leak`: nested branch-local name does not poison a following implicit self call (exit code 42);
- `sibling-branch-shadow`: branch-local name in `then` never shadows the corresponding `else` call (exit code 42);
- `index-binder-does-not-leak`: `for (let int read = ...)` shadows only its own loop, not a subsequent method call (exit code 42);
- `scoped-shadow-in-branch`, `nested-branch-shadow`, `index-binder-shadows-in-loop`: live lexical scalar binders defeat the method-family lookup and require explicit `self.read()` (negative compiler cases, no IR);
- `shadowed-parameter`: still rejected;
- `sealed-actor-not-ready`: still denied until verified sealed state, critical loan and actor runtime lowering.

The LLVM native subset has no claim of actor-state execution, native inheritance parity, or unrestricted lexical closures. Differential Java checks require exact PR-head results and must distinguish unsupported language features from successful equivalence.

## Release and CI gate

Require exact source head/tree, actual nonzero executed GitHub Actions steps, real native compile/link/execute under **both O0 and O2** for every positive fixture (release and ASan/UBSan builds), Java differential outputs when supported, and independent review. A sanitizer report must fail even when the guest process exits 42; require both registered native CTest modes to execute. A workflow with zero executed steps is runner-admission failure, not compiler evidence. Private cross-organization mirrors are forbidden unless the source repository's independently reviewed exact carrier authorization applies to this head and test plan.
