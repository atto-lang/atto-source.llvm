#!/usr/bin/env bash
set -euo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
H="$R/verification/liveview-fingerprint-host"
python3 "$H/verify_tsv_against_fixture.py" "$R/contracts/v1/ores-liveview-event-fingerprint-v1.json" "$H/fingerprint-v1.tsv"
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
"${CXX:-clang++}" -std=c++20 -Wall -Wextra -Werror -pedantic -O1 -g \
  -fsanitize=address,undefined "$H/liveview_fingerprint_vectors.cpp" -lcrypto -o "$D/liveview-fingerprint-host"
"$D/liveview-fingerprint-host" "$H/fingerprint-v1.tsv"
