#!/bin/sh
# Standalone, source-local, HOST-ONLY ownership CFG adversarial test.
# Does not require LLVM, Rust, Python or private runner mirroring.
# No source files are published or copied outside this checkout except the
# single temporary compiled local test executable.
set -eu
cd "$(dirname "$0")/.."
umask 077
scratch=$(mktemp -d "${TMPDIR:-/tmp}/ores-ownership-cfg-phi.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT HUP INT TERM
cxx="${CXX:-c++}"
"$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -Isrc/main/cpp tests/ownership_cfg_phi_loan_adversarial_test.cpp \
  -o "$scratch/ownership-cfg-phi"
"$scratch/ownership-cfg-phi"
echo "HOST_CFG_ONLY: no typed-MIR source generation, LLVM SSA, AOT or guest proof"
