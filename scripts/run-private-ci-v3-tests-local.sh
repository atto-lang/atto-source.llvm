#!/usr/bin/env bash
# Offline, read-only V3 regression acceptance from an exact, clean LLVM PR HEAD.
# This runner NEVER invokes the publisher CLI or exports private source.
set -euo pipefail

expected_head="${1:-}"
if [[ ! "$expected_head" =~ ^[0-9a-f]{40}$ ]]; then
  echo 'USAGE: run-private-ci-v3-tests-local.sh <exact-40-char-PR-head-SHA>' >&2
  exit 64
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"

test "$(git rev-parse --show-toplevel)" = "$root" || {
  echo 'DENY: script must run from a Git checkout root' >&2; exit 65;
}
actual_head="$(git rev-parse --verify HEAD)"
test "$actual_head" = "$expected_head" || {
  echo "DENY: checkout HEAD $actual_head differs from expected $expected_head" >&2
  exit 65
}

git diff --quiet --exit-code
git diff --cached --quiet --exit-code
test -z "$(git ls-files --others --exclude-standard)" || {
  echo 'DENY: checkout contains untracked nonignored files' >&2; exit 65;
}

for path in \
  scripts/publish-private-ci-source-v3.py \
  scripts/test-publish-private-ci-source-v3.py \
  scripts/private_ci_export_gate.py \
  tests/test_private_ci_export_gate.py \
  scripts/audit-private-ci-governance.py \
  scripts/test-audit-private-ci-governance.py; do
  test -f "$path" && test ! -L "$path" || {
    echo "DENY: required regular source-free test file unavailable: $path" >&2
    exit 65
  }
done

tmp_dir="$(mktemp -d)"
trap 'rm -rf -- "$tmp_dir"' EXIT
mkdir -m 700 "$tmp_dir/home" "$tmp_dir/config"

# No credentials, connector, gh configuration, or proxy context is propagated.
clean_env=(env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$tmp_dir/home"
  XDG_CONFIG_HOME="$tmp_dir/config" LC_ALL=C.UTF-8
  PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=scripts)

run_suite() {
  local title="$1" logfile="$2"
  shift 2
  echo "SOURCE_FREE_SUITE_BEGIN=$title"
  "${clean_env[@]}" python3 "$@" 2>&1 | tee "$logfile"
  grep -Eq '^Ran [1-9][0-9]* tests? in ' "$logfile" || {
    echo "DENY: no executed unittest count for $title" >&2; exit 66;
  }
  grep -qx 'OK' "$logfile" || {
    echo "DENY: unittest did not report OK for $title" >&2; exit 66;
  }
  echo "SOURCE_FREE_SUITE_PASS=$title"
}

printf 'V3_EXACT_HEAD=%s\n' "$actual_head"
printf 'V3_EXACT_TREE=%s\n' "$(git rev-parse 'HEAD^{tree}')"
printf 'PYTHON_VERSION=%s\n' "$("${clean_env[@]}" python3 --version 2>&1)"
for path in scripts/publish-private-ci-source-v3.py scripts/test-publish-private-ci-source-v3.py; do
  printf 'V3_TEST_INPUT_BLOB=%s %s\n' "$(git hash-object "$path")" "$path"
done

run_suite standalone "$tmp_dir/standalone.log" \
  -m unittest discover -s tests -p test_private_ci_export_gate.py -v
run_suite v3-publisher "$tmp_dir/v3.log" \
  scripts/test-publish-private-ci-source-v3.py -v
run_suite governance "$tmp_dir/governance.log" \
  scripts/test-audit-private-ci-governance.py -v

printf 'V3_SOURCE_FREE_ALL_SUITES_PASS head=%s tree=%s\n' \
  "$actual_head" "$(git rev-parse 'HEAD^{tree}')"
