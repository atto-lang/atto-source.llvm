#!/usr/bin/env bash
set -euo pipefail
generator=$(cd "$(dirname "$0")/../cmake" && pwd)/AttoLibraryEmbed.cmake
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/stdlib/core" "$work/stdlib/alloc" "$work/stdlib/std" "$work/core"
printf 'define module num as\nend\n' > "$work/stdlib/core/num.atto"
cp "$work/stdlib/core/num.atto" "$work/stdlib/alloc/arena.atto"
cp "$work/stdlib/core/num.atto" "$work/stdlib/std/path.atto"
cp "$work/stdlib/core/num.atto" "$work/stdlib/std/path_posix.atto"
cp "$work/stdlib/core/num.atto" "$work/stdlib/std/path_windows.atto"
cp "$work/stdlib/core/num.atto" "$work/core/range.atto"
ln -s "$work/stdlib/core/num.atto" "$work/stdlib/core/alias.atto"
cat > "$work/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(EmbedAudit NONE)
include("${GENERATOR}")
atto_generate_library_table("${CMAKE_CURRENT_BINARY_DIR}")
CMAKE
run() {
  printf '%s\n' "$2" > "$work/stdlib/LIBRARIES.txt"
  rm -rf "$work/build"
  if cmake -S "$work" -B "$work/build" -DGENERATOR="$generator" > "$work/log" 2>&1; then
    result=accept
  else
    result=reject
  fi
  if [[ "$result" != "$1" ]]; then
    cat "$work/log"
    echo "unexpected $result: $2" >&2
    exit 1
  fi
  if [[ "$result" == reject ]]; then
    grep -q 'stdlib/LIBRARIES.txt:' "$work/log"
    test ! -e "$work/build/oreslang/frontend/GeneratedAttoLibraryTable.inc"
  fi
}
run accept $'core num stdlib/core/num.atto any\ncore range core/range.atto any\nalloc arena stdlib/alloc/arena.atto any\nstd path stdlib/std/path_posix.atto posix\nstd path stdlib/std/path_windows.atto windows'
for bad in \
 'core num stdlib/core/num.atto unix' \
 'core num stdlib/core/num.atto any allocator' \
 $'core num stdlib/core/num.atto any\ncore num stdlib/core/num.atto any' \
 $'std path stdlib/std/path_posix.atto posix\nstd path stdlib/std/path.atto any' \
 'core num stdlib/core/../core/num.atto any' \
 'core num stdlib/alloc/arena.atto any' \
 'core alias stdlib/core/alias.atto any' \
 "core $(printf 'a%.0s' {1..65}) stdlib/core/num.atto any"; do
 run reject "$bad"
done
# Both registrations are individually valid; ambiguity must fail in either order.
run reject $'std path stdlib/std/path.atto any\nstd path stdlib/std/path_posix.atto posix'
grep -q "overlapping unit std/path" "$work/log"
echo 'Library embedding: positive manifest and nine fail-closed controls passed'
