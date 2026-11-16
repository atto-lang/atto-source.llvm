#!/usr/bin/env bash
# Real .atto -> typed frontend -> LLVM IR -> freestanding Cortex-M3 ELF -> QEMU.
# No libc, CRT, malloc, OS, OS threads, or guest std. The test-only semihost
# handler merely transfers the guest's observed return value to the runner.
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_NET_OFFLINE=true

CLANG="${CLANG:-clang-18}"
command -v "$CLANG" >/dev/null || CLANG=clang
command -v "$CLANG" >/dev/null || { echo "MISSING clang" >&2; exit 2; }
command -v qemu-system-arm >/dev/null || { echo "MISSING qemu-system-arm" >&2; exit 2; }
command -v readelf >/dev/null || { echo "MISSING readelf" >&2; exit 2; }
command -v timeout >/dev/null || { echo "MISSING timeout" >&2; exit 2; }
"$CLANG" --version | head -n1
qemu-system-arm --version | head -n1

compiler="${ATTO_LLVM:-target/release/atto-llvm}"
if [[ ! -x "$compiler" ]]; then
  cargo build --offline --release --bin atto-llvm
fi
test -x "$compiler"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

arch=(-target armv7m-none-eabi -mcpu=cortex-m3 -mthumb)
no_runtime=(-ffreestanding -fno-builtin -fno-stack-protector -fno-exceptions
            -fno-unwind-tables -fno-asynchronous-unwind-tables)
"$CLANG" "${arch[@]}" "${no_runtime[@]}" -x c -c \
    tests/freestanding/cortexm_boot.c -o "$work/boot.o"

verify_elf() {
  local elf="$1" name="$2"
  readelf -W -h "$elf" | grep -Eq 'Machine:[[:space:]]+ARM'
  readelf -W -s "$elf" | awk '
    $7 == "UND" && NF >= 8 && $8 != "" {
      print "UNEXPECTED_UNDEFINED: " $8 > "/dev/stderr"; bad=1
    }
    END { exit bad ? 1 : 0 }'
  if readelf -W -l "$elf" | grep -Eq '(^|[[:space:]])(INTERP|DYNAMIC)([[:space:]]|$)'; then
    echo "FAIL: $name has hosted ELF loader/dynamic dependency" >&2; exit 1
  fi
  readelf -W -S "$elf" | grep -q '\.isr_vector'
  readelf -W -s "$elf" | grep -q 'atto_test_vectors'
  readelf -W -s "$elf" | grep -q 'Reset_Handler'
  readelf -W -s "$elf" | grep -q ' main$'
  if readelf -W -r "$elf" | grep -Eq 'R_ARM_.*(GLOB|JUMP|COPY)'; then
    echo "FAIL: $name has dynamic relocation" >&2; exit 1
  fi
  sha256sum "$elf"
}

run_guest() {
  local name="$1" expected="$2"
  local source="tests/freestanding/$name.atto"
  local ir="$work/$name.ll" guest="$work/$name.o"
  local elf="$work/$name.elf"
  "$compiler" "$source" -o "$ir" --emit-llvm --freestanding-core
  test -s "$ir"
  grep -q 'define i32 @main()' "$ir"
  if grep -Eq '@(atto_actor_|malloc|free|printf|puts|fopen|pthread|__wasi|fd_write)' "$ir"; then
    echo "FAIL: guest IR has forbidden hosted service" >&2; exit 1
  fi
  "$CLANG" "${arch[@]}" "${no_runtime[@]}" -O1 -x ir -c "$ir" -o "$guest"
  # LLVM 18 emits an R_ARM_NONE reference to a personality routine in
  # discarded .ARM.exidx metadata even with -fno-unwind-tables. Remove only
  # exception tables from relocatable objects, then strip unneeded undefined
  # symbols. This must never remove a *live* code relocation or define a fake
  # unwinder; the linked ELF and guest object are audited below.
  local guest_core="$work/$name.core.o" boot_core="$work/boot.core.o"
  llvm-objcopy-18 --remove-section=.ARM.exidx --remove-section=.ARM.extab \
      --strip-unneeded "$guest" "$guest_core"
  llvm-objcopy-18 --remove-section=.ARM.exidx --remove-section=.ARM.extab \
      --strip-unneeded "$work/boot.o" "$boot_core"
  readelf -W -s "$guest_core" | awk '
    $7 == "UND" && NF >= 8 && $8 != "" {
      print "GUEST_OBJECT_UNDEFINED: " $8 > "/dev/stderr"; bad=1
    }
    END { exit bad ? 1 : 0 }'
  "$CLANG" "${arch[@]}" -nostdlib -fuse-ld=lld \
      -Wl,-T,tests/freestanding/cortexm_flash.ld -Wl,--no-undefined -Wl,--gc-sections \
      -Wl,-Map,"$work/$name.map" "$boot_core" "$guest_core" -o "$elf"
  verify_elf "$elf" "$name"
  set +e
  timeout 25s qemu-system-arm -M lm3s6965evb -nographic \
      -monitor none -serial none -semihosting-config enable=on,target=native \
      -kernel "$elf" >"$work/$name.qemu.log" 2>&1
  local status=$?
  set -e
  cat "$work/$name.qemu.log"
  if [[ "$status" != "$expected" ]]; then
    echo "FAIL: $name qemu exit=$status expected=$expected" >&2; exit 1
  fi
  if [[ "$status" == 124 || "$status" == 137 ]]; then
    echo "FAIL: timeout/killed guest" >&2; exit 1
  fi
  echo "PASS: real .atto guest $name -> Cortex-M3 ELF -> QEMU exit $status"
}

# Negative control must return a distinct error, proving exit is based on
# emitted guest behavior and not blindly forced to success by the harness.
run_guest pass-42 0
run_guest fail-41 1

for name in deny-stdio deny-unbound deny-alloc deny-process deny-std-import; do
  case "$name" in
    deny-stdio|deny-process) diagnostic="E_FREESTANDING_HOST_EFFECT" ;;
    deny-unbound) diagnostic="E_UNBOUND" ;;
    deny-alloc) diagnostic="E_ALLOCATOR_REQUIRED" ;;
    deny-std-import) diagnostic="E_FREESTANDING_FORBIDDEN_IMPORT" ;;
    *) exit 9 ;;
  esac
  artifact="$work/$name.ll"
  if "$compiler" "tests/freestanding/$name.atto" -o "$artifact" --emit-llvm --freestanding-core \
      >"$work/$name.compiler.log" 2>&1; then
    echo "FAIL: accepted forbidden/unbound source $name" >&2; exit 1
  fi
  cat "$work/$name.compiler.log"
  grep -q "$diagnostic" "$work/$name.compiler.log"
  test ! -e "$artifact" || { echo "FAIL: negative case emitted LLVM IR"; exit 1; }
  test ! -e "$work/$name.elf" || { echo "FAIL: negative case emitted ELF"; exit 1; }
  echo "PASS: negative no-artifact $name"
done

echo "FREESTANDING_CORTEXM_GUEST_PASS positive_executed=2 negative_no_artifact=5 libc=0 os=0 allocator=0"
