#!/usr/bin/env bash
# REAL Atto -> typed LLVM IR -> clang -> native ELF guard proof.
# No host-only source checker, no pre-compiled fixture, no artifact on reject.
set -euo pipefail
cd "$(dirname "$0")/.."
compiler=target/release/atto-llvm
test -x "$compiler"
command -v clang >/dev/null
command -v readelf >/dev/null
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
accepted=0
rejected=0
run_ok() {
  local name="$1" expect="$2"
  local src="$work/$name.atto" output="$work/$name.exe" status
  cat > "$src"
  "$compiler" "$src" -o "$output"
  test -s "$output"
  readelf -h "$output" | grep -q 'Class:[[:space:]]*ELF64'
  set +e
  (ulimit -c 0; timeout 15s "$output")
  status=$?
  set -e
  if [[ "$status" != "$expect" ]]; then
    echo "FAIL guard guest $name status=$status expected=$expect" >&2
    return 1
  fi
  echo "PASS guest $name exit=$status blob=$(git hash-object "$src")"
  accepted=$((accepted+1))
}
run_reject() {
  local name="$1" diag="$2"
  local src="$work/$name.atto" output="$work/$name.exe" status
  cat > "$src"
  set +e
  "$compiler" "$src" -o "$output" > "$work/$name.stderr" 2>&1
  status=$?
  set -e
  if [[ "$status" == 0 || -e "$output" ]] || { [[ -n "$diag" ]] && ! grep -Fq "$diag" "$work/$name.stderr"; }; then
    echo "FAIL guard rejection $name status=$status diag=$diag" >&2
    cat "$work/$name.stderr" >&2
    return 1
  fi
  echo "PASS no-artifact $name blob=$(git hash-object "$src")"
  rejected=$((rejected+1))
}
run_ok brace-true 42 <<'ATTO'
pub fnc main(): int { guard true { return 7; } return 42; }
ATTO
run_ok keyword-true 42 <<'ATTO'
pub fnc main(): int { guard true else return 7; end return 42; }
ATTO
run_ok brace-false 7 <<'ATTO'
pub fnc main(): int { guard false { return 7; } return 42; }
ATTO
run_ok keyword-false 7 <<'ATTO'
pub fnc main(): int { guard false else return 7; end return 42; }
ATTO
run_ok keyword-nested-if 42 <<'ATTO'
pub fnc main(): int {
  if true then
    guard true else return 7; end
  else
    return 9;
  fi
  return 42;
}
ATTO
run_ok keyword-nested-return 7 <<'ATTO'
pub fnc main(): int {
  guard false else
    if true then return 7; else return 9; fi
  end
  return 42;
}
ATTO
run_ok keyword-lazy-comma 7 <<'ATTO'
fnc checked(bool p, int value): bool { return p && value + 1 > 0; }
pub fnc main(): int {
  guard false, checked(true, 9223372036854775807) else return 7; end
  return 42;
}
ATTO
run_ok keyword-caret-return 7 <<'ATTO'
pub fnc main(): int { guard false else ^ 7; end return 42; }
ATTO
run_ok brace-two-tests 7 <<'ATTO'
pub fnc main(): int { guard true, false { return 7; } return 42; }
ATTO
run_ok explicit-lazy-operators 42 <<'ATTO'
fnc overflow_if_called(int value): bool { return value + 1 > 0; }
fnc and_case(bool a): bool { return a && overflow_if_called(9223372036854775807); }
fnc or_case(bool a): bool { return a || overflow_if_called(9223372036854775807); }
pub fnc main(): int {
  guard !and_case(false), or_case(true), true || false && false { return 7; }
  return 42;
}
ATTO
run_ok mixed-explicit-guard 42 <<'ATTO'
pub fnc main(): int {
  guard true, false || true, true && true else return 7; end
  return 42;
}
ATTO
run_ok brace-caret-return 7 <<'ATTO'
pub fnc main(): int { guard false { ^ 7; } return 42; }
ATTO
run_ok brace-outer-else 42 <<'ATTO'
pub fnc main(): int {
    if true then guard true { return 7; } else return 9; fi
    return 42;
}
ATTO
run_reject infix-single-caret "" <<'ATTO'
pub fnc main(): int { return 1 ^ 2; }
ATTO
run_reject nonvoid-caret-void "" <<'ATTO'
pub fnc main(): int { guard false { ^ void; } return 42; }
ATTO
run_reject nonbool-lazy-and E_TYPE <<'ATTO'
pub fnc main(): int { guard false && 42 { return 7; } return 42; }
ATTO
run_reject nonbool-lazy-or E_TYPE <<'ATTO'
pub fnc main(): int { guard true || 42 else return 7; end return 42; }
ATTO
run_reject brace-fallthrough E_GUARD <<'ATTO'
pub fnc main(): int { guard true { val x = 1; } return 42; }
ATTO
run_reject keyword-fallthrough E_GUARD <<'ATTO'
pub fnc main(): int { guard true else val x = 1; end return 42; }
ATTO
run_reject keyword-nonbool E_TYPE <<'ATTO'
pub fnc main(): int { guard 17 else return 7; end return 42; }
ATTO
run_reject keyword-wrong-return E_TYPE <<'ATTO'
pub fnc main(): int { guard false else return true; end return 42; }
ATTO
run_reject brace-break E_GUARD <<'ATTO'
pub fnc main(): int { loop { guard false { break; } return 7; } return 42; }
ATTO
run_reject keyword-continue E_GUARD <<'ATTO'
pub fnc main(): int { loop { guard false else continue; end return 7; } return 42; }
ATTO
run_reject keyword-throw E_GUARD <<'ATTO'
pub fnc main(): int { guard false else throw Error("bad"); end return 42; }
ATTO
run_reject keyword-hidden-break E_GUARD <<'ATTO'
pub fnc main(): int { loop { guard false else break; return 7; end return 42; } }
ATTO
run_reject keyword-missing-end "" <<'ATTO'
pub fnc main(): int { guard true else return 7; fi return 42; }
ATTO
test "$accepted" -eq 13
test "$rejected" -eq 13
echo "GUARD_DUAL_FORM_NATIVE_AOT accepted=$accepted rejected=$rejected real_compiler=yes compiled_guest=yes"
