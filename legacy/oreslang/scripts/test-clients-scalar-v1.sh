#!/usr/bin/env bash
# Exact-source nine Oreslang clients scalar fixtures. Real LLVM/Clang execution.
# Not proof of HTTP/JSON/module/actor/full SDK support, nor Java/Wasm/.NET parity.
set -Eeuo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd -- "$root"
compiler="$root/build/oreslang-llvmc"
[[ -f "$compiler" && -x "$compiler" && ! -L "$compiler" ]] || {
  echo 'Build a trusted real oreslang-llvmc before running these guest cases.' >&2
  exit 2
}
command -v clang >/dev/null || { echo 'Missing clang' >&2; exit 2; }
sha256sum --check --status <<'CHECKSUMS'
e42464360ffa360b263bc74f74ef485e6a9e4eb9dff601ead2c4576982622576  tests/fixtures/clients-scalar-v1/01-precedence.ores
9196a61de9c4710bdf32e658de396c521ad2abb2e3d03ce711ae449ac7aa5a2f  tests/fixtures/clients-scalar-v1/02-grouping.ores
613d1e1e104361ce555f397dd0d23754d44ade5b80750147110c1e4a73cce286  tests/fixtures/clients-scalar-v1/03-signed.ores
b0f80c76a24cb15ed25fee0bdaa096d7cbcd318f787f7d8a040166c127532247  tests/fixtures/clients-scalar-v1/04-i64-edge.ores
2bcfdc119d3e48b9ac97e2061b7b88571d6e621b04ecabf886347d9eeb19382c  tests/fixtures/clients-scalar-v1/05-separators.ores
dd1bf333cdd17140571227e810427030a279666b8a4d06eb5968a056cf5a8a2a  tests/fixtures/clients-scalar-v1/06-nested.ores
c45493f0814253ab5a2e8896d8b39643c7b8e1247d408eef11baccd79d6580e1  tests/fixtures/clients-scalar-v1/07-unknown-reject.ores
37b793a36e231ee37d953b8359cd0f004dc008a795eb32a413762a0a504c542c  tests/fixtures/clients-scalar-v1/08-overflow-reject.ores
32f4b30c52ce58955ecbb0aa0d564f0ac394737940faaab01351e25ed9df4b75  tests/fixtures/clients-scalar-v1/09-syntax-reject.ores
CHECKSUMS

work="$(mktemp -d "$root/build/clients-scalar.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
src='tests/fixtures/clients-scalar-v1'
for pair in '01-precedence:14' '02-grouping:20' '03-signed:5' \
            '04-i64-edge:1' '05-separators:100' '06-nested:35'; do
  name="${pair%%:*}"
  expected="${pair#*:}"
  artifact="$work/$name"
  bash scripts/compile-aot.sh "$src/$name.ores" "$artifact" > "$work/compiler.stdout"
  [[ -f "$artifact" && -x "$artifact" && ! -L "$artifact" ]] || {
    echo "No native executable emitted for $name" >&2
    exit 1
  }
  if "$artifact" > "$work/guest.stdout" 2> "$work/guest.stderr"; then
    exit_code=0
  else
    exit_code=$?
  fi
  [[ "$exit_code" == "$expected" ]] || {
    echo "Wrong native guest exit for $name: expected $expected, got $exit_code" >&2
    exit 1
  }
  [[ ! -s "$work/guest.stdout" && ! -s "$work/guest.stderr" ]] || {
    echo "Guest emitted unexpected output: $name" >&2
    exit 1
  }
done

# Compiler failures must be frontend denials, not unrecognized crashes or
# inability to locate the compiler. IR and native artifact must remain absent.
for name in 07-unknown-reject 08-overflow-reject 09-syntax-reject; do
  ir="$work/$name.ll"
  artifact="$work/$name"
  if "$compiler" --target=host "$src/$name.ores" >"$ir" 2>"$work/deny.stderr"; then
    echo "Invalid source admitted by LLVM frontend: $name" >&2
    exit 1
  fi
  [[ ! -s "$ir" && -s "$work/deny.stderr" ]] || {
    echo "LLVM frontend published IR or omitted a diagnostic: $name" >&2
    exit 1
  }
  if grep -Eqi 'segmentation fault|stack trace|panic|internal compiler error|assertion failed' "$work/deny.stderr"; then
    echo "LLVM frontend crashed rather than rejecting: $name" >&2
    exit 1
  fi
  if bash scripts/compile-aot.sh "$src/$name.ores" "$artifact" >"$work/deny-compile.stdout" 2>"$work/deny-compile.stderr"; then
    echo "Invalid source published native executable: $name" >&2
    exit 1
  fi
  [[ ! -e "$artifact" ]] || {
    echo "Rejected source left a native artifact: $name" >&2
    exit 1
  }
done
echo 'LLVM_CLIENT_SCALAR_6_NATIVE_RESULTS_3_COMPILER_DENIALS'
