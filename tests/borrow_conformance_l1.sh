#!/usr/bin/env bash
# L1 borrow conformance: run the canonical ownership-graph corpora through the
# native OwnershipCfg.hpp engine and require every non-conforming verdict to be
# a listed, explained known difference. Host-only (no compiled .atto guests).
#
#   tests/borrow_conformance_l1.sh            # verify pins, build, check both corpora
#   CXX=g++ tests/borrow_conformance_l1.sh
set -euo pipefail
cd "$(dirname "$0")/.."
dir=contracts/borrow-conformance
for corpus in graph-cases-v1.json graph-cases-hybrid-v1.json; do
  want=$(sed -n "s/^${corpus}=//p" "$dir/PIN")
  have=$(git hash-object --no-filters "$dir/$corpus")
  if [ "$want" != "$have" ]; then
    echo "pin mismatch for $corpus: PIN=$want file=$have" >&2
    exit 1
  fi
done
mkdir -p build/borrow-conformance-l1
bin=build/borrow-conformance-l1/adapter
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Wpedantic -Werror -O1 -g \
  ${SANITIZE:+-fsanitize=address,undefined -fno-sanitize-recover=all} \
  -I src/main/cpp tests/borrow_conformance_l1_adapter.cpp -o "$bin"
status=0
for corpus in graph-cases-v1 graph-cases-hybrid-v1; do
  # Results for atto-borrow-conform graph-results (cross-repo L1 evidence).
  "$bin" "$dir/$corpus.json" > "build/borrow-conformance-l1/$corpus.llvm.jsonl"
  echo "== $corpus"
  "$bin" "$dir/$corpus.json" --check "$dir/$corpus.llvm-known.tsv" || status=1
done
exit $status
