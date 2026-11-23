#!/usr/bin/env bash
# Native .atto imported-source execution, not concatenated guest source or host simulation.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(git hash-object -- stdlib/http/message_policy.atto)" == "f88b78313432bce067223cc1853024bfd9b97c90" ]] || {
  echo "FAIL: compiler-owned LLVM std.http source blob has drifted" >&2; exit 1;
}
[[ "$(git hash-object -- tests/http_stdlib/namespace_import_canary.atto)" == "4a63850b44189a77f7bed1dc9e83f3596187c6a0" ]] || {
  echo "FAIL: imported HTTP guest fixture blob has drifted" >&2; exit 1;
}
cargo generate-lockfile --offline
cargo build --locked --offline --release --bin atto-llvm
compiler="$(pwd)/target/release/atto-llvm"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

positive="$work/namespace_import_canary.atto"
cp tests/http_stdlib/namespace_import_canary.atto "$positive"
# A malicious *ambient* file with the same name as the compiler registry
# must never substitute for the packaged .atto source embedded at build time.
mkdir -p "$work/std/http"
printf 'pub fnc http_status_valid(int n): bool { return false; }\n' \
  > "$work/std/http/message_policy.atto"
(
  cd "$work"
  "$compiler" "$positive" -o "$work/native-http-import.exe"
)
test -s "$work/native-http-import.exe" || {
  echo "FAIL: imported source did not produce a native executable" >&2; exit 1;
}
file "$work/native-http-import.exe"
set +e
timeout 15s "$work/native-http-import.exe" >"$work/positive.stdout" 2>"$work/positive.stderr"
status=$?
set -e
if [[ "$status" -ne 42 || -s "$work/positive.stdout" || -s "$work/positive.stderr" ]]; then
  echo "FAIL: imported HTTP guest exited $status; expected silent exit 42" >&2
  cat "$work/positive.stdout" "$work/positive.stderr" >&2
  exit 1
fi
echo "ATTO_LLVM_HTTP_NAMESPACE_GUEST_PASS cases=17 exit=42 compiler_owned=true"

# The negative canaries must all fail at native compilation, return a
# diagnostic specific to the denied condition, and leave NO output artifact.
reject() {
  local label="$1" diagnostic="$2" profile="hosted"
  if [[ $# -gt 2 ]]; then profile="$3"; fi
  local source="$work/$label.atto" artifact="$work/$label.exe"
  [[ -s "$source" ]] || { echo "FAIL: missing negative $label" >&2; exit 1; }
  rm -f -- "$artifact"
  local result=0
  if [[ "$profile" == freestanding ]]; then
    "$compiler" "$source" -o "$artifact" --emit-llvm --freestanding-core \
      >"$work/$label.stdout" 2>"$work/$label.stderr" || result=$?
  else
    "$compiler" "$source" -o "$artifact" \
      >"$work/$label.stdout" 2>"$work/$label.stderr" || result=$?
  fi
  [[ "$result" -ne 0 ]] || { echo "FAIL: $label compiled despite a denied import" >&2; exit 1; }
  [[ ! -e "$artifact" && ! -s "$work/$label.stdout" ]] || {
    echo "FAIL: $label emitted artifact or guest output" >&2; exit 1;
  }
  grep -Fq -- "$diagnostic" "$work/$label.stderr" || {
    echo "FAIL: $label returned unrelated compiler error" >&2
    cat "$work/$label.stderr" >&2
    exit 1
  }
  echo "ATTO_LLVM_HTTP_NAMESPACE_NEGATIVE_PASS case=$label diagnostic=$diagnostic"
}
cat >"$work/unknown_builtin.atto" <<'ATTO'
import * as http from "std.unknown";
pub fnc main(): int { return 42; }
ATTO
reject unknown_builtin E_NATIVE_IMPORT_UNREGISTERED

# Unproved generic Copy bounds in core collections must not become
# a native import just because the source file exists in the repository.
cat >"$work/unproven_core_collections.atto" <<'ATTO'
import * as collections from "ores.core.collections";
pub fnc main(): int { return 42; }
ATTO
reject unproven_core_collections E_NATIVE_IMPORT_UNREGISTERED

cat >"$work/private_member.atto" <<'ATTO'
import * as http from "std.http";
pub fnc main(): int { return http.private_helper(200); }
ATTO
reject private_member E_NATIVE_IMPORT_PRIVATE

cat >"$work/unqualified_builtin.atto" <<'ATTO'
import * as http from "std.http";
pub fnc main(): int { if http_status_valid(200) then return 42; fi return 1; }
ATTO
reject unqualified_builtin E_NATIVE_IMPORT_PRIVATE

cat >"$work/duplicate_alias.atto" <<'ATTO'
import * as http from "std.http";
import * as http from "std.http";
pub fnc main(): int { return 42; }
ATTO
reject duplicate_alias E_NATIVE_IMPORT_UNSUPPORTED

cat >"$work/unsupported_module.atto" <<'ATTO'
import module http3 from "std.http";
pub fnc main(): int { return 42; }
ATTO
reject unsupported_module E_NATIVE_IMPORT_UNSUPPORTED

cat >"$work/bare_alias.atto" <<'ATTO'
import * as http from "std.http";
pub fnc main(): int { val v = http; return 42; }
ATTO
reject bare_alias E_NATIVE_IMPORT_ALIAS

cat >"$work/freestanding.atto" <<'ATTO'
import * as http from "std.http";
pub fnc main(): int { return 42; }
ATTO
reject freestanding E_FREESTANDING_FORBIDDEN_IMPORT freestanding

cat >"$work/no_import.atto" <<'ATTO'
pub fnc main(): int { if http_status_valid(200) then return 42; fi return 1; }
ATTO
reject no_import E_UNBOUND

echo 'ATTO_LLVM_HTTP_NAMESPACE_NEGATIVES_ALL_PASS checks=9'
