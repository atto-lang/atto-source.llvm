#!/usr/bin/env bash
# AttoLang M6: source-local native HOST verification (no checkout transfer).
# This does NOT execute a compiler-produced .atto guest or attest GitHub CI.
set -euo pipefail
umask 077

root="$(cd "$(git rev-parse --show-toplevel)" && pwd -P)"
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
# Validate the canonical destination BEFORE creating it: an inside-repo
# OUTPUT_DIR must not leave an untracked directory in the verified checkout.
out_candidate="$(python3 - "$root" "$OUTPUT_DIR" <<'PY'
from pathlib import Path
import sys
root, proposed = (Path(x).resolve() for x in sys.argv[1:])
if proposed == root or root in proposed.parents:
    raise SystemExit("REFUSED: output must be outside source checkout")
print(proposed)
PY
)"
mkdir -p "$out_candidate"
out="$(cd "$out_candidate" && pwd -P)"
if [[ -n "$(ls -A "$out")" ]]; then
  echo "REFUSED: OUTPUT_DIR must be empty; never reuse earlier logs/receipts" >&2
  exit 2
fi

# These are the six current #1521 tracked sources, helpers, and witness, plus
# the existing quota-policy test. Reject a moved head or modified test bytes.
files=(
  "src/main/cpp/oreslang/runtime/LocalContextDomainsV1.hpp:3ee0121b59927b9a0b40a4bce73e0902c7d1a2bc"
  "src/main/cpp/oreslang/runtime/ActorLocalSchedulerTurnV1.hpp:e3d7826945390ce0e89f11d73764cf2351a59683"
  "src/main/cpp/oreslang/runtime/LocalValueAdmissionV1.hpp:7b79138951aa878fe2ee43cfcef21277cb68b256"
  "src/main/cpp/oreslang/runtime/LocalPayloadBudgetV1.hpp:bcecb54e7750fca7ef84010a2fa3f66e6bf61902"
  "tests/native_context_domains_v1_test.cpp:c9fd993bd78e4a6b2bd27c384fff43276ca915dd"
  "tests/native_actorlocal_stop_during_copy_v1_test.cpp:d21b6c525819c277038f2c5f0d393c3b82cf2f35"
  "tests/native_actorlocal_stop_mutation_witness.sh:d80313a05287185ff7b39fef395d58d6f8514b71"
  "tests/native_local_read_revocation_v1_test.cpp:cf49e51bed5834fae1d76a1191ec18e41f379656"
  "tests/native_local_payload_budget_v1_test.cpp:25dec8d5f15bafbc6d28d73f94c9027f21ca9ac5"
  "tests/native_local_read_revocation_mutation_witness.sh:0d256fe89fd963e520226f7207f434b9e6477be7"
)
# Live scheduler/authority dependencies are pinned by the exact HEAD
# tree, not a floating branch name. Check their working blobs below.
for dep in src/main/cpp/oreslang/runtime/NativeActorScheduler.c \
           src/main/cpp/oreslang/runtime/NativeActorScheduler.h \
           src/main/cpp/oreslang/runtime/NativeExecutionAuthority.h; do
  files+=("$dep:$(git rev-parse "HEAD:$dep")")
done
for entry in "${files[@]}"; do
  path="${entry%%:*}"
  expected="${entry#*:}"
  if [[ ! -f "$path" ]]; then
    echo "REFUSED: missing native source/test file $path" >&2
    exit 2
  fi
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
  native_actorlocal_stop_during_copy_v1_test
)
case "$CXX" in
  g++) CC=gcc ;;
  clang++) CC=clang ;;
  *) echo "REFUSED: expected a supported GCC/Clang C++ compiler" >&2; exit 2 ;;
esac
"$CXX" --version > "$out/compiler-version.txt"
"$CC" --version > "$out/c-compiler-version.txt"
"$CC" -std=c11 "${flags[@]:1}" -Isrc/main/cpp \
  -c src/main/cpp/oreslang/runtime/NativeActorScheduler.c \
  -o "$work/scheduler.o" >"$out/scheduler.compile.log" 2>&1 || {
    cat "$out/scheduler.compile.log" >&2
    echo "HOST_ONLY_COMPILE_FAIL: native scheduler" >&2
    exit 1
  }
for name in "${tests[@]}"; do
  linked=()
  if [[ "$name" == native_actorlocal_stop_during_copy_v1_test ]]; then
    linked=("$work/scheduler.o")
  fi
  "$CXX" "${flags[@]}" -Isrc/main/cpp "tests/$name.cpp" "${linked[@]}" \
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
  CXX="$CXX" bash tests/native_actorlocal_stop_mutation_witness.sh \
    >"$out/actorlocal-stop-mutation.log" 2>&1 || {
      cat "$out/actorlocal-stop-mutation.log" >&2
      echo "STOP_MUTATION_WITNESS_MISSING_OR_UNEXPECTED" >&2
      exit 1
    }
fi

# This JSON reports only the local host-native source and test invocations.
# It is explicitly NON-PROMOTING. Real CI and compiler-produced .atto guests
# still need independently read-back, exact-head workflow/job receipts.
python3 - "$out" "$work" "$head" "$tree" "$CXX" "$MODE" "${files[@]}" <<'PY'
import datetime, hashlib, json, pathlib, sys
out, work, head, tree, compiler, mode, *pins = sys.argv[1:]
out, work = pathlib.Path(out), pathlib.Path(work)
tests = ["native_context_domains_v1_test", "native_local_read_revocation_v1_test",
         "native_local_payload_budget_v1_test", "native_actorlocal_stop_during_copy_v1_test"]
logs = {}
for path in sorted(out.glob("*.log")):
    logs[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
binaries = {name: hashlib.sha256((work / name).read_bytes()).hexdigest()
            for name in tests}
source_pins = dict(pin.split(":", 1) for pin in pins)
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
    "compileExitCodes": {name: 0 for name in tests},
    "hostExitCodes": {name: 0 for name in tests},
    "binarySha256": binaries,
    "verifiedGitBlobPins": source_pins,
    "mutationWitness": "caught" if mode == "optimized" else "not-run",
    "actorStopMutations": ["postcopy", "stopgate"] if mode == "optimized" else [],
    "logsSha256": logs,
    "notes": "Runner admission and guest parity remain independent requirements."
}
(out / "host-local-receipt.json").write_text(json.dumps(record, indent=2) + "\n")
print("LOCAL_HOST_ONLY_NOT_ATTESTED receipt="+str(out/"host-local-receipt.json"))
PY
