#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-c++}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
reject() {
  local name="$1" body="$2" diagnostic="$3"
  printf '#include "oreslang/runtime/DopInvertedColumns.hpp"\n#include <string>\n#include <vector>\n%s\n' "$body" > "$tmp/bad.cpp"
  if "$cxx" -std=c++20 -I"$here/src/main/cpp" "$tmp/bad.cpp" -o "$tmp/bad" 2>"$tmp/error"; then
    echo "FAIL: unsafe inverted column source was accepted: $name" >&2
    exit 1
  fi
  if ! grep -Fq "$diagnostic" "$tmp/error"; then
    echo "FAIL: negative gate '$name' did not fail for the intended reason" >&2
    cat "$tmp/error" >&2
    exit 1
  fi
  echo "PASS negative: $name"
}
reject "pointer-bearing field" \
  'int main() { oreslang::dop::InvertedColumns<int*> invalid; (void)invalid.size(); }' \
  'phase-one columns only support pointer-free arithmetic and enum fields'
reject "heap-owning std::string field" \
  'int main() { oreslang::dop::InvertedColumns<std::string> invalid; (void)invalid.size(); }' \
  'phase-one columns only support pointer-free arithmetic and enum fields'
reject "heap-owning std::vector field" \
  'int main() { oreslang::dop::InvertedColumns<std::vector<float>> invalid; (void)invalid.size(); }' \
  'phase-one columns only support pointer-free arithmetic and enum fields'
reject "mutable packed bool projection" \
  'int main() { oreslang::dop::InvertedColumns<bool> a; auto w = a.borrow_write<0>(); (void)w.span(); }' \
  'bool byte lanes require canonical writes via set'
printf '#include "oreslang/runtime/DopInvertedColumns.hpp"\nint main() { oreslang::dop::detail::InvertedOwnerSequence::next_.store(1); }\n' > "$tmp/private-owner.cpp"
if "$cxx" -std=c++20 -I"$here/src/main/cpp" "$tmp/private-owner.cpp" -o "$tmp/private-owner" 2>"$tmp/private-owner.err"; then
  echo "FAIL: host code mutated the inverted owner sequence" >&2
  exit 1
fi
if ! grep -Fq 'private' "$tmp/private-owner.err"; then
  echo "FAIL: owner-sequence gate failed for an unexpected reason" >&2
  cat "$tmp/private-owner.err" >&2
  exit 1
fi
echo "PASS negative: host cannot mutate owner sequence"
printf '#include "oreslang/runtime/DopInvertedColumns.hpp"\n#include <cstdint>\nenum class Status : std::uint32_t { Idle=0, Ready=1 };\nint main() { oreslang::dop::InvertedColumns<Status> a; auto h=a.push(Status::Ready); return a.get<0>(h) != Status::Ready; }\n' > "$tmp/good.cpp"
"$cxx" -std=c++20 -Wall -Wextra -Werror -I"$here/src/main/cpp" "$tmp/good.cpp" -o "$tmp/good"
"$tmp/good"
echo "PASS positive: fixed-width nominal enum"
