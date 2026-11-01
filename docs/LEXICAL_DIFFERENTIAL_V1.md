# LLVM lexical conformance: execution-gated differential adapter v1

Status: **DRAFT / NOT_EXECUTED**. This is a native compiler integration contract,
not an assertion that the LLVM lexer matches Java or has passed any fixture.

## Exact shared input

- \`tests/contracts/editor-syntax/v1/compiler-lexical-differential-v1.tsv\`:
  34 source cases; Git blob \`eeed64b7d33c262b35120662386435575982a9bc\`.
- Input strings are UTF-16 hex *code units* to retain illegal/unpaired input.
  They must be converted to the native frontend's chosen input encoding
  losslessly or rejected with an independently checked encoding diagnostic.
  Do not treat UTF-8-decoding errors as proof of proper Java Symbol handling.
- Java's 21-method focused suite provides expected \`Token.Type\` sequences,
  normalized Symbol names and error diagnostics; it is still unexecuted because
  the private Actions runner has failed before any steps.
- A separate public-origin transport adapter exists in
  \`oreslang-common-source/scripts/llvm-lexical-probe-adapter.mjs\`.
  Its public CI tests use an explicitly synthetic mock driver. These tests
  prove *transport and fail-closed handling*, not LLVM compiler correctness.

## Inspected production frontend limitations

The current \`src/main/cpp/oreslang/frontend/Parser.cpp\` has an
anonymous-namespace \`Lexer\` returning a coarse \`Kind\` enum:
\`Identifier, Integer, String, Template, Symbol, End\`. This is NOT the
Java compiler's \`Token.Type\` vocabulary. It has no public JSON lexical
diagnostic interface.

Specific observed gaps in source (NOT live execution results):

1. \`identifier_start(char)\` is ASCII-oriented. Java uses
   \`Character.isLetter(char)\` for BMP Unicode letters, so
   \`éreturn\`, \`returné\`, and \`中eq\` need a deliberate UTF-8 frontend
   migration rather than a synthetic \`IDENT\` claim.
2. The native lexer recognizes \`nb-select\` as one identifier-like token;
   Java splits this spelling into \`NB\` and \`SELECT\`. Parser compatibility
   does not establish lexer token-sequence parity.
3. The native source scanner has no Java-equivalent \`#...\` Symbol admission
   and NFC normalizer. A native parser failing an invalid Symbol source is NOT
   sufficient to prove that it accepts a valid one.
4. Its punctuation/token rules are narrower (e.g. \`^\` does not have Java's
   \`CARET\` token representation) and its template-comment depth limits
   differ from the hardened Java source.
5. Source fragment compilation through \`oreslang-llvmc\` is not equivalent
   to lexical admission: many fixtures are not standalone valid programs.
   Inferred parser success/failure must not satisfy a lexical oracle.

## Native driver interface to implement

The common-source adapter requires an explicit opt-in CLI that does not yet
exist in \`oreslang-llvmc\`. Adding it must call the **production frontend
lexer**, not a duplicate reference tokenizer.

Protocol (safe subprocess, no shell or GitHub secrets):

- \`oreslang-llvmc --oreslang-lexical-v1-capabilities\` prints exactly
  \`ORESLANG_LEXICAL_V1\` (plus newline) and exits 0 only when available.
- \`oreslang-llvmc --oreslang-lexical-v1-case\` accepts a bounded one-line
  JSON payload on stdin containing \`id\` and \`sourceUtf16Hex\`.
- The driver returns one JSON object for the same \`id\`, with
  \`outcome: "TOKENS"\`, \`tokenTypes\` in Java lexical-v1 names, and
  \`symbolNameUtf16Hex\` (or \`null\`), OR \`outcome: "ERROR"\` and a
  bounded \`errorMessage\`.
- Rejected input and Unsupported are not \`PASS\`. If lexer translation
  cannot be implemented without semantics changes, fail closed with
  \`INTERFACE_UNAVAILABLE\` and record the missing feature.
- The public case comparator requires all 34 unique IDs, exact corpus Git
  blob, exact token sequence/Symbol/error evidence, and will surface
  bounded per-case mismatches. Its output is explicitly
  \`UNTRUSTED_COMPARISON_ONLY\`.

## Promotion gates

1. Expose the production native lexer via a size-, nesting- and time-bounded
   lexical-v1 driver; preserve source offsets and encoding errors.
2. Run the 34-case corpus using that exact native compiler binary. Capture
   toolchain version, source commit, immutable binary hash and actual job logs.
3. Add 4,096/4,097 nested-comment and 1,024 UTF-16-unit Symbol boundary
   regressions. Do not silently claim parity for unsupported cases.
4. Compare outputs against *executed* Java JUnit at its exact source head.
   Require independent reviewer approval and private Actions eligibility.
5. Promote the shared six-backend matrix status (Java, LLVM, WASM, Cranelift, .NET, Forge) only after checking actual
   runner steps and per-case output. A plausible JSON receipt alone is not
   evidence.

No private compiler source has been exported to an unapproved organization.
The adapter contract and shared fixture contain synthetic, nonsecret inputs.

## Native nested-comment depth boundary (candidate; needs execution)

The LLVM production frontend previously had no nesting-depth limit for
ordinary `/* */` comments or block comments in template interpolations.
This draft now rejects a 4,097th nested `/*` at source admission (limit 4,096),
aligning the boundary with the pinned Java v1 lexer. Two CTests invoke
`oreslang-llvmc` with 4,096/4,097-level source fixtures. The valid
case must emit `ret i64 42`; the invalid case must reject with the
specific depth diagnostic and must not produce valid LLVM IR. Template
interpolation uses the same guard, but does not yet have an independently
executed depth-boundary fixture.

These source changes are **NOT a lexical-v1 JSON probe** and do not establish
native Unicode, Symbol normalization, or parser semantic parity. Require
a current exact-PR-head CTest job with assigned runner, executed steps and
the exact two named tests before making a pass claim.
