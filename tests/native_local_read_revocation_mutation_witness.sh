#!/usr/bin/env bash
# Source-local, host-only negative mutation witness. Never edits checkout.
# A missing post-copy lifetime fence must be detected by the real C++ test.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
CXX="\${CXX:-g++}"
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
if [[ "$status" -eq 0 ]]; then
  echo "MUTATION_UNDETECTED: unguarded read unexpectedly passed" >&2
  exit 1
fi
if ! grep -Fq '!returned' "$work/mutation.log"; then
  echo "MUTATION_INVALID: failed for a reason other than revoked read assertion" >&2
  cat "$work/mutation.log" >&2
  exit 1
fi
printf 'LOCAL_CONTEXT_READ_REVOCATION_MUTATION_CAUGHT compiler=%s\n' "$CXX"
