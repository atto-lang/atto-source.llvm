#!/usr/bin/env bash
# AttoLang M6: source-local native HOST verification (no checkout transfer).
# This does NOT execute a compiler-produced .atto guest or attest GitHub CI.
set -euo pipefail
umask 077

root="$(git rev-parse --show-toplevel)"
cd "$root"
: "${EXPECTED_HEAD:?set reviewed 40-character PR head SHA}"
: "${EXPECTED_TREE:?set reviewed 40-character Git root tree SHA}"
: "${OUTPUT_DIR:?set an output directory OUTSIDE the repository}"
CXX="${CXX:-g++}"
MODE="${MODE:-optimized}"
case "$MODE" in optimized|asan-ubsan|tsan) ;; *) echo "Unsupported MODE=$MODE" >&2; exit 2;; esac
[[ "$EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || { echo "Invalid EXPECTED_HEAD" >&2; exit 2; }
[[ "$EXPECTED_TREE" =~ ^[0-9a-f]{40}$ ]] || { echo "Invalid EXPECTED_TREE" >&2; exit 2; }
[[ -z "$(git status --porcelain --untracked-files=all)" ]] || {
  echo "REFUSED: checkout must be clean, including untracked files" >&2; exit 2;
}
head="$(git rev-parse HEAD)"
tree="$(git rev-parse 'HEAD^{tree}')"
[[ "$head" == "$EXPECTED_HEAD" && "$tree" == "$EXPECTED_TREE" ]] || {
  echo "REFUSED: exact PR head or root tree mismatch" >&2; exit 2;
}
command -v "$CXX" >/dev/null || { echo "Missing C++ compiler: $CXX" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required for receipt hashing" >&2; exit 2; }
command -v git >/dev/null || exit 2
mkdir -p "$OUTPUT_DIR"
out="$(cd "$OUTPUT_DIR" && pwd -P)"
case "$out/" in "$root/"*) echo "REFUSED: output must be outside source checkout" >&2; exit 2;; esac
rm -f "$out/host-local-receipt.json"  # stale success cannot survive failed execution

# These are the six current #1521 tracked sources, helpers, and witness, plus
# the existing quota-policy test. Reject a moved head or modified test bytes.
files=(
  "src/main/cpp/oreslang/runtime/LocalContextDomainsV1.hpp:3ee0121b59927b9a0b40a4bce73e0902c7d1a2bc"
  "src/main/cpp/oreslang/runtime/LocalValueAdmissionV1.hpp:7b79138951aa878fe2ee43cfcef21277cb68b256"
  "src/main/cpp/oreslang/runtime/LocalPayloadBudgetV1.hpp:bcecb54e7750fca7ef84010a2fa3f66e6bf61902"
  "tests/native_context_domains_v1_test.cpp:c9fd993bd78e4a6b2bd27c384fff43276ca915dd"
  "tests/native_local_read_revocation_v1_test.cpp:cf49e51bed5834fae1d76a1191ec18e41f379656"
  "tests/native_local_payload_budget_v1_test.cpp:25dec8d5f15bafbc6d28d73f94c9027f21ca9ac5"
  "tests/native_local_read_revocation_mutation_witness.sh:40f5b62235c5aab64b5861252bf0fceab701cd3d"
)
for entry in "${files[@]}"; do
  path="${entry%%:*}"
  expected="${entry#*:}"
  actual="$(git hash-object "$path")"
  tracked="$(git rev-parse "HEAD:$path")"
  if [[ "$actual" != "$expected" || "$tracked" != "$expected" ]]; then
    echo "REFUSED: source/test Git blob mismatch at $path" >&2
    exit 2
  fi
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
flags=(-std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread -UNDEBUG)
case "$MODE" in
 optimized) flags+=(-O2);;
 asan-ubsan) flags+=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
             export ASAN_OPTIONS='detect_leaks=1:halt_on_error=1'
             export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1';;
 tsan) flags+=(-O1 -g -fno-omit-frame-pointer -fsanitize=thread)
       export TSAN_OPTIONS='halt_on_error=1';;
esac

tests=(
  native_context_domains_v1_test
  native_local_read_revocation_v1_test
  native_local_payload_budget_v1_test
)
"$CXX" --version > "$out/compiler-version.txt"
for name in "${tests[@]}"; do
  "$CXX" "${flags[@]}" -Isrc/main/cpp "tests/$name.cpp" \
    -o "$work/$name" >"$out/$name.compile.log" 2>&1 || {
       cat "$out/$name.compile.log" >&2
       echo "HOST_ONLY_COMPILE_FAIL: $name" >&2
       exit 1
    }
  "$work/$name" >"$out/$name.run.log" 2>&1 || {
     cat "$out/$name.run.log" >&2
     echo "HOST_ONLY_RUNTIME_FAIL: $name" >&2
     exit 1
  }
  echo "HOST_ONLY_EXECUTED_PASS: $name compiler=$CXX mode=$MODE"
done

if [[ "$MODE" == optimized ]]; then
  # The negative control must fail *at the expected revoked-read assertion*
  # after deleting the safety check in a temporary copy; no source is edited.
  CXX="$CXX" bash tests/native_local_read_revocation_mutation_witness.sh \
    >"$out/revocation-mutation.log" 2>&1 || {
      cat "$out/revocation-mutation.log" >&2
      echo "MUTATION_WITNESS_MISSING_OR_UNEXPECTED" >&2
      exit 1
    }
fi

# This JSON reports only the local host-native source and test invocations.
# It is explicitly NON-PROMOTING. Real CI and compiler-produced .atto guests
# still need independently read-back, exact-head workflow/job receipts.
python3 - "$out" "$head" "$tree" "$CXX" "$MODE" <<'PY'
import datetime, hashlib, json, pathlib, subprocess, sys
out, head, tree, compiler, mode = sys.argv[1:]
out = pathlib.Path(out)
tests = ["native_context_domains_v1_test", "native_local_read_revocation_v1_test",
         "native_local_payload_budget_v1_test"]
files = {}
for path in sorted(out.glob("*.log")):
    files[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
record = {
    "schemaVersion": 1,
    "classification": "LOCAL_HOST_ONLY_NOT_ATTESTED",
    "canPromote": False,
    "compilerBackedGuest": False,
    "hostedGitHubActions": False,
    "repo": "ores-truffle-oreslang/oreslang-source.llvm",
    "head": head, "tree": tree,
    "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "compiler": compiler, "mode": mode,
    "executedHostPrograms": tests,
    "mutationWitness": "caught" if mode == "optimized" else "not-run",
    "logsSha256": files,
    "notes": "Runner admission and guest parity remain independent requirements."
}
(out / "host-local-receipt.json").write_text(json.dumps(record, indent=2) + "\n")
print("LOCAL_HOST_ONLY_NOT_ATTESTED receipt="+str(out/"host-local-receipt.json"))
PY
