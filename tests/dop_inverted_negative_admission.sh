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
  'phase-one columns only support i32/i64/f32/f64/bool or FixedRecord fields'
reject "heap-owning std::string field" \
  'int main() { oreslang::dop::InvertedColumns<std::string> invalid; (void)invalid.size(); }' \
  'phase-one columns only support i32/i64/f32/f64/bool or FixedRecord fields'
reject "heap-owning std::vector field" \
  'int main() { oreslang::dop::InvertedColumns<std::vector<float>> invalid; (void)invalid.size(); }' \
  'phase-one columns only support i32/i64/f32/f64/bool or FixedRecord fields'
reject "mutable packed bool projection" \
  'int main() { oreslang::dop::InvertedColumns<bool> a; (void)a.borrow_write<0>(); }' \
  'bool/record byte lanes require canonical writes via set'
reject "mutable fixed-record projection" \
  'int main() { using R = oreslang::dop::FixedRecord<std::int32_t>; oreslang::dop::InvertedColumns<R> a; (void)a.borrow_write<0>(); }' \
  'bool/record byte lanes require canonical writes via set'
reject "nested fixed record member" \
  'int main() { using Inner = oreslang::dop::FixedRecord<std::int32_t>; using Outer = oreslang::dop::FixedRecord<Inner>; (void)Outer::encoded_size; }' \
  'fixed record members must be i32/i64/f32/f64/bool'
reject "cv-qualified scalar column" \
  'int main() { oreslang::dop::InvertedColumns<const std::int32_t> invalid; (void)invalid.size(); }' \
  'phase-one columns reject cv-qualified field types'
reject "cv-qualified fixed record member" \
  'int main() { using R = oreslang::dop::FixedRecord<const std::int32_t>; (void)R::encoded_size; }' \
  'fixed record members must be i32/i64/f32/f64/bool'
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
reject "unsigned scalar outside shared ABI" \
  'int main() { oreslang::dop::InvertedColumns<std::uint32_t> invalid; (void)invalid.size(); }' \
  'phase-one columns only support i32/i64/f32/f64/bool or FixedRecord fields'
reject "narrow signed scalar outside shared ABI" \
  'int main() { oreslang::dop::InvertedColumns<std::int16_t> invalid; (void)invalid.size(); }' \
  'phase-one columns only support i32/i64/f32/f64/bool or FixedRecord fields'
reject "nominal enum without shared ABI" \
  'enum class Status : std::uint32_t { Idle=0, Ready=1 }; int main() { oreslang::dop::InvertedColumns<Status> invalid; (void)invalid.size(); }' \
  'phase-one columns only support i32/i64/f32/f64/bool or FixedRecord fields'
printf '#include "oreslang/runtime/DopInvertedColumns.hpp"\n#include <cstdint>\nint main() { oreslang::dop::InvertedColumns<std::int32_t, std::int64_t, float, double, bool> a; auto h=a.push(1,2,3.0f,4.0,true); return a.get<0>(h) != 1; }\n' > "$tmp/good.cpp"
"$cxx" -std=c++20 -Wall -Wextra -Werror -I"$here/src/main/cpp" "$tmp/good.cpp" -o "$tmp/good"
"$tmp/good"
echo "PASS positive: exact shared scalar ABI"
