#!/usr/bin/env bash
# SOURCE-FREE driver/publication regression. This uses a synthetic LLVM IR
# emitter, NOT the real AttoLang frontend or a real guest-AOT parity receipt.
# Linux-only: requires Bash, Clang and coreutils. Real source execution is
# separately required by CTest native_aot_driver.
set -euo pipefail
[[ $# == 2 ]] || { echo 'usage: test-compile-aot-publication.sh DRIVER CLANG' >&2; exit 64; }
driver="$1"
clang="$2"
[[ "$(uname -s)" == Linux && -f "$driver" && -x "$clang" ]] || {
  echo 'Linux driver and real Clang are required.' >&2
  exit 69
}
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
source="$work/program.atto"
output="$work/native app"

cat > "$work/fake-llvmc" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[[ $# == 2 && "$1" == --target=host ]] || exit 64
case "$(cat "$2")" in
  *'return 42;'*) number=42 ;;
  *'return 7;'*) number=7 ;;
  *) echo 'synthetic compiler source denied' >&2; exit 65 ;;
esac
printf 'define i32 @main() { ret i32 %s }\n' "$number"
MOCK
chmod +x "$work/fake-llvmc"

cat > "$work/fake-linker" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
out=''
while (( $# )); do
  if [[ "$1" == -o ]]; then shift; out="$1"; fi
  shift
done
[[ -n "$out" ]] || exit 64
case "$FAKE_OUTPUT_KIND" in
  empty) : > "$out" ;;
  shell) printf '#!/bin/sh\nexit 42\n' > "$out" ;;
  forged-elf|forged-class|forged-noexec|forged-range|forged-phoff|forged-entry-gap)
    dd if=/dev/zero of="$out" bs=1 count=256 status=none
    printf '\177ELF\002\001\001' | dd of="$out" bs=1 seek=0 conv=notrunc status=none
    # A deliberately forged ET_DYN ELF64 with an executable PT_LOAD:
    # each case corrupts one independently necessary admission condition.
    printf '\003\000' | dd of="$out" bs=1 seek=16 conv=notrunc status=none
    case "$(uname -m)" in
      x86_64) printf '\076\000' ;;
      aarch64|arm64) printf '\267\000' ;;
      *) exit 69 ;;
    esac | dd of="$out" bs=1 seek=18 conv=notrunc status=none
    printf '\001\000\000\000' | dd of="$out" bs=1 seek=20 conv=notrunc status=none
    printf '\100\000\000\000\000\000\000\000' | dd of="$out" bs=1 seek=32 conv=notrunc status=none
    printf '\100\000\070\000\001\000' | dd of="$out" bs=1 seek=52 conv=notrunc status=none
    printf '\001\000\000\000\005\000\000\000' | dd of="$out" bs=1 seek=64 conv=notrunc status=none
    printf '\200\000\000\000\000\000\000\000' | dd of="$out" bs=1 seek=72 conv=notrunc status=none
    printf '\040\000\000\000\000\000\000\000' | dd of="$out" bs=1 seek=96 conv=notrunc status=none
    printf '\040\000\000\000\000\000\000\000' | dd of="$out" bs=1 seek=104 conv=notrunc status=none
    if [[ "$FAKE_OUTPUT_KIND" != forged-entry-gap ]]; then
      printf '\000\020\000\000\000\000\000\000' |
        dd of="$out" bs=1 seek=80 conv=notrunc status=none
    fi
    if [[ "$FAKE_OUTPUT_KIND" != forged-elf ]]; then
      printf '\000\020\000\000\000\000\000\000' |
        dd of="$out" bs=1 seek=24 conv=notrunc status=none
    fi
    case "$FAKE_OUTPUT_KIND" in
      forged-class)
        printf '\001' | dd of="$out" bs=1 seek=4 conv=notrunc status=none ;;
      forged-noexec)
        printf '\004\000\000\000' | dd of="$out" bs=1 seek=68 conv=notrunc status=none ;;
      forged-range)
        printf '\377\377\377\177' | dd of="$out" bs=1 seek=72 conv=notrunc status=none ;;
      forged-phoff)
        printf '\001\000\000\000' | dd of="$out" bs=1 seek=36 conv=notrunc status=none ;;
    esac
    ;;
  *) exit 64 ;;
esac
chmod +x "$out"
MOCK
chmod +x "$work/fake-linker"

run_exit() {
  local expected="$1" status
  status=0
  "$output" || status=$?
  [[ "$status" == "$expected" ]] || {
    echo "Synthetic native executable returned $status (expected $expected)" >&2
    exit 1
  }
}

printf 'define pub fnc main() -> int { return 42; }\n' > "$source"
ORESLANG_LLVMC="$work/fake-llvmc" CLANG="$clang" bash "$driver" "$source" "$output"
[[ -s "$output" && -x "$output" ]]
run_exit 42
first="$(sha256sum "$output" | cut -d' ' -f1)"

printf 'define pub fnc main() -> int { return 7; }\n' > "$source"
ORESLANG_LLVMC="$work/fake-llvmc" CLANG="$clang" bash "$driver" "$source" "$output"
run_exit 7
second="$(sha256sum "$output" | cut -d' ' -f1)"
[[ "$first" != "$second" ]] || {
  echo 'Distinct synthetic programs reused one executable.' >&2
  exit 1
}

printf 'define pub fnc main() -> int { return 42; }\n' > "$source"
for kind in empty shell forged-elf forged-class forged-noexec forged-range forged-phoff forged-entry-gap; do
  if ORESLANG_LLVMC="$work/fake-llvmc" CLANG="$work/fake-linker" \
      FAKE_OUTPUT_KIND="$kind" bash "$driver" "$source" "$output" \
      > "$work/$kind.stdout" 2> "$work/$kind.stderr"; then
    echo "Published fake linker artifact: $kind" >&2
    exit 1
  fi
  [[ "$(sha256sum "$output" | cut -d' ' -f1)" == "$second" ]] || {
    echo "Failed $kind link clobbered the previous native executable." >&2
    exit 1
  }
done

printf 'this is deliberately not valid AttoLang\n' > "$source"
if ORESLANG_LLVMC="$work/fake-llvmc" CLANG="$clang" \
   bash "$driver" "$source" "$work/rejected" > "$work/rejected.log" 2>&1; then
  echo 'Rejected synthetic source emitted an executable.' >&2
  exit 1
fi
[[ ! -e "$work/rejected" ]] || {
  echo 'Rejected synthetic source left a native release artifact.' >&2
  exit 1
}

echo "PASS: SOURCE-FREE AOT driver 42/7, malformed linker artifact denial, old-binary preservation."
echo "NOT A GUEST-AOT COMPILER OR CROSS-PLATFORM RELEASE RECEIPT."
