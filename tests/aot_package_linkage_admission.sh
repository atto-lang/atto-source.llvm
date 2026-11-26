#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
driver="$root/scripts/compile-aot.sh"
w="$(mktemp -d)"; trap 'rm -rf "$w"' EXIT
printf 'previous valid output\n' > "$w/prior"
cp "$w/prior" "$w/output"
printf '#!/bin/sh\ntouch "%s"\nexit 99\n' "$w/invoked" > "$w/compiler"
chmod +x "$w/compiler"
for mode in dynamic hybrid unknown; do
  code=0
  ORESLANG_LLVMC="$w/compiler" bash "$driver" "$w/nonexistent.atto" "$w/output" "--link=$mode" 2>"$w/error" || code=$?
  [[ "$code" == 64 ]]
  if [[ "$mode" == unknown ]]; then grep -Fq 'E_LINK_MODE:' "$w/error"; else grep -Fq 'E_LINK_MODE_UNSUPPORTED:' "$w/error"; fi
  cmp "$w/prior" "$w/output"
  test ! -e "$w/invoked"
  code=0
  ORESLANG_LLVMC="$w/compiler" bash "$driver" "$w/nonexistent.atto" "$w/new" "--link=$mode" 2>"$w/error" || code=$?
  [[ "$code" == 64 && ! -e "$w/new" && ! -e "$w/invoked" ]]
done
echo 'PASS host driver denies unsupported/invalid modes without touching compiler or output'
# Source-free integration: synthetic IR producer, real Clang/LLD and ELF
# screening. Assert the FINAL link receives the selected flags, not the probe.
real_clang="$(command -v clang)"
export REAL_CLANG="$real_clang" LINK_ARGS="$w/link-args"
cat > "$w/ir-producer" <<'SH'
#!/usr/bin/env bash
printf 'define i32 @main() { ret i32 42 }\n'
SH
cat > "$w/auditor" <<'SH'
#!/usr/bin/env bash
test -s "$1"
SH
cat > "$w/clang-wrapper" <<'SH'
#!/usr/bin/env bash
set -eu
for arg in "$@"; do
  if [[ "$arg" == */program.ll ]]; then
    printf '%s\n' "$@" > "$LINK_ARGS"
    break
  fi
done
exec "$REAL_CLANG" "$@"
SH
chmod +x "$w/ir-producer" "$w/auditor" "$w/clang-wrapper"
printf 'source-free IR producer fixture\n' > "$w/stub.atto"
for opt in 0 2; do
  for lto in off full thin; do
    rm -f "$LINK_ARGS"
    ORESLANG_LLVMC="$w/ir-producer" ORES_NO_GC_IR_AUDIT="$w/auditor" \
      CLANG="$w/clang-wrapper" ORESLANG_LLVM_AOT_OPT_LEVEL="$opt" \
      ORESLANG_AOT_LTO="$lto" bash "$driver" "$w/stub.atto" "$w/native" --link=static
    grep -Fxq -- "-O$opt" "$LINK_ARGS"
    case "$lto" in
      off) if grep -Eq '^(-flto|-flto=thin|-fuse-ld=lld)
  printf 'pub fnc main(): int { return 42; }\n' > "$w/main.atto"
  bash "$driver" "$w/main.atto" "$w/default"
  bash "$driver" "$w/main.atto" "$w/static" --link=static
  ORESLANG_AOT_OBJECT_PIPELINE=separate ORESLANG_AOT_LTO=off bash "$driver" "$w/main.atto" "$w/object-static" --link=static
  for exe in default static object-static; do
    code=0; "$w/$exe" || code=$?
    [[ "$code" == 42 ]]
  done
  echo 'PASS actual LLVM compiler native guest default, explicit static and separate object return 42'
else
  echo 'RECEIPT actual_guest=NOT_EXECUTED'
fi
 "$LINK_ARGS"; then exit 1; fi ;;
      full) grep -Fxq -- -flto "$LINK_ARGS"; grep -Fxq -- -fuse-ld=lld "$LINK_ARGS" ;;
      thin) grep -Fxq -- -flto=thin "$LINK_ARGS"; grep -Fxq -- -fuse-ld=lld "$LINK_ARGS" ;;
    esac
    code=0; "$w/native" || code=$?
    [[ "$code" == 42 ]]
    echo "PASS synthetic IR final-link flags and native execution opt=$opt lto=$lto"
  done
done
# Run the same synthetic input through the actual two-stage object pipeline.
for opt in 0 2; do
  ORESLANG_LLVMC="$w/ir-producer" ORES_NO_GC_IR_AUDIT="$w/auditor" \
    CLANG="$w/clang-wrapper" ORESLANG_AOT_LTO=off ORESLANG_AOT_OBJECT_PIPELINE=separate \
    ORESLANG_LLVM_AOT_OPT_LEVEL="$opt" bash "$driver" "$w/stub.atto" "$w/separate" --link=static
  grep -Fxq -- -c "$LINK_ARGS"
  code=0; "$w/separate" || code=$?
  [[ "$code" == 42 ]]
  echo "PASS synthetic IR separate object pipeline opt=$opt"
done
code=0
ORESLANG_AOT_LTO=full ORESLANG_AOT_OBJECT_PIPELINE=separate \
  bash "$driver" "$w/stub.atto" "$w/unsupported" --link=static 2>"$w/error" || code=$?
[[ "$code" == 64 && ! -e "$w/unsupported" ]]
grep -Fq 'E_OBJECT_PIPELINE_UNSUPPORTED:' "$w/error"
# Fail closed on output symlinks, dangling symlinks, FIFOs and directories.
ln -s "$w/native" "$w/symlink"
ln -s "$w/nonexistent" "$w/dangling"
mkfifo "$w/fifo"
mkdir "$w/directory"
sha256sum "$w/native" > "$w/native.sha"
for dest in symlink dangling fifo directory; do
  code=0
  ORESLANG_LLVMC="$w/ir-producer" ORES_NO_GC_IR_AUDIT="$w/auditor" \
    CLANG="$w/clang-wrapper" bash "$driver" "$w/stub.atto" "$w/$dest" --link=static 2>"$w/error" || code=$?
  [[ "$code" == 64 ]]
  grep -Fq 'E_ARTIFACT_PUBLICATION:' "$w/error"
done
sha256sum -c "$w/native.sha"
[[ ! -e "$w/nonexistent" && -L "$w/dangling" && -p "$w/fifo" && -d "$w/directory" ]]
echo 'PASS nonregular destinations rejected; original executable preserved'
if [[ "${1:-}" == --guest ]]; then
  printf 'pub fnc main(): int { return 42; }\n' > "$w/main.atto"
  bash "$driver" "$w/main.atto" "$w/default"
  bash "$driver" "$w/main.atto" "$w/static" --link=static
  for exe in default static; do
    code=0; "$w/$exe" || code=$?
    [[ "$code" == 42 ]]
  done
  echo 'PASS actual LLVM compiler native guest default and explicit static return 42'
else
  echo 'RECEIPT actual_guest=NOT_EXECUTED'
fi
