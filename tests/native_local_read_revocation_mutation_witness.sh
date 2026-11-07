#!/usr/bin/env bash
# Source-local, host-only negative mutation witness. Never edits checkout.
# A missing post-copy lifetime fence must be detected by the real C++ test.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
CXX="${CXX:-g++}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
ulimit -c 0 || true
mkdir -p "$work/include/oreslang/runtime"
for header in LocalContextDomainsV1.hpp LocalPayloadBudgetV1.hpp LocalValueAdmissionV1.hpp; do
  cp "src/main/cpp/oreslang/runtime/$header" "$work/include/oreslang/runtime/$header"
done
python3 - "$work/include/oreslang/runtime/LocalContextDomainsV1.hpp" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
guard = """        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!open_) return std::nullopt;
        }
        return snapshot;
"""
if s.count(guard) != 1:
    raise SystemExit("mutation witness cannot locate exactly one logical read guard")
p.write_text(s.replace(guard, "        return snapshot;\n", 1))
PY
"$CXX" -std=c++17 -O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$work/include" tests/native_local_read_revocation_v1_test.cpp \
  -o "$work/unguarded-host-local"
status=0
("$work/unguarded-host-local") >"$work/mutation.log" 2>&1 || status=$?
# A compiler crash, SIGSEGV (139), timeout (124), or arbitrary failing
# command is NOT proof that deleting the fence was detected. The expected
# source assertion terminates via SIGABRT (128 + 6 = 134 on this Linux CI).
if [[ "$status" -ne 134 ]]; then
  echo "MUTATION_INVALID_EXIT: expected revoked-read SIGABRT(134), observed $status" >&2
  cat "$work/mutation.log" >&2
  exit 1
fi
# Require all three markers on the same assertion line. A generic log that
# happens to mention '!returned' must never count as negative mutation proof.
if ! grep -F 'native_local_read_revocation_v1_test.cpp:' "$work/mutation.log" \
    | grep -F 'Assertion' | grep -Fq '!returned'; then
  echo "MUTATION_INVALID_DIAGNOSTIC: missing exact revoked-read assertion" >&2
  cat "$work/mutation.log" >&2
  exit 1
fi
printf 'LOCAL_CONTEXT_READ_REVOCATION_MUTATION_CAUGHT compiler=%s\n' "$CXX"
