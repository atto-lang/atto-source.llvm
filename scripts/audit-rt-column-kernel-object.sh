#!/usr/bin/env sh
# Constrained object-only audit, NOT whole linked-image, FFI, GC or WCET proof.
# Demand explicit failures on unresolved allocator/copy/throw dependencies.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cxx=${CXX:-c++}
nm_tool=${NM:-nm}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$cxx" -std=c++17 -O2 -fno-exceptions -fno-rtti \
  -Wall -Wextra -Wpedantic -Werror \
  -I "$root/src/main/cpp" -c "$root/tests/rt_column_kernel_object_probe.cpp" \
  -o "$tmp/kernel.o"
"$nm_tool" -u "$tmp/kernel.o" > "$tmp/undefined.txt"
# Only the existing arena's exact live-turn guard is permitted as an external
# dependency. Unrecognized symbols fail closed (rather than silently passing
# new intrinsics, lock providers or allocation dependencies).
if ! grep -Eq '(^|[[:space:]])_?ores_rt_actor_arena_read$' "$tmp/undefined.txt"; then
  echo "missing mandatory live-turn guard reference" >&2
  cat "$tmp/undefined.txt" >&2
  exit 1
fi
if grep -Ev '^[[:space:]]*U[[:space:]]+_?ores_rt_actor_arena_read$|^[[:space:]]*$' \
     "$tmp/undefined.txt" > "$tmp/unexpected.txt"; then
  echo "unapproved unresolved dependency in RT kernel object" >&2
  cat "$tmp/unexpected.txt" >&2
  exit 1
fi
echo 'PASS: isolated RT kernel object has only live-turn guard external (NOT image or WCET proof)'
