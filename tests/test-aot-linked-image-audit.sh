#!/usr/bin/env bash
# Synthetic ELF controls only: NEVER claims executed .atto guest coverage.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
auditor="${ORES_LINK_AUDITOR:-$root/scripts/audit-aot-linked-image.sh}"
clang="${CLANG:-clang}"
command -v "$clang" >/dev/null || { echo 'clang required' >&2; exit 69; }
for tool in objcopy readelf; do command -v "$tool" >/dev/null || { echo "$tool required" >&2; exit 69; }; done
[[ -f "$auditor" ]] || { echo 'linked-image auditor missing' >&2; exit 69; }
work="$(mktemp -d)"; trap 'rm -rf -- "$work"' EXIT
cat >"$work/clean.c" <<'C'
#include <stdlib.h>
int main(void) { void *p = malloc(24); if (!p) return 1; free(p); return 0; }
C
cat >"$work/data.c" <<'C'
#include <string.h>
const char gc_name[] = "GC_malloc";
int main(void) { return strcmp(gc_name, "GC_malloc"); }
C
cat >"$work/collector.c" <<'C'
#include <stdlib.h>
void *GC_malloc(size_t n) { return malloc(n); }
int main(void) { void *p = GC_malloc(24); free(p); return !p; }
C
cat >"$work/collector2.c" <<'C'
void gc_collect(void) {}
int main(void) { gc_collect(); return 0; }
C
cat >"$work/loader.c" <<'C'
#include <dlfcn.h>
int main(void) { return dlopen(0, RTLD_NOW) == 0; }
C
cat >"$work/foreign.c" <<'C'
int foreign_x(void) { return 1; }
C
cat >"$work/foreign_main.c" <<'C'
extern int foreign_x(void);
int main(void) { return foreign_x() != 1; }
C
"$clang" -O0 "$work/clean.c" -o "$work/clean"
"$clang" -O0 "$work/data.c" -o "$work/data"
"$clang" -O0 "$work/collector.c" -o "$work/collector"
"$clang" -O0 "$work/collector2.c" -o "$work/collector2"
"$clang" -O0 "$work/loader.c" -ldl -o "$work/loader"
"$clang" -O0 -s "$work/clean.c" -o "$work/stripped"
# GNU readelf may return exit 0 while reporting a malformed .symtab. A fake
# section must never satisfy the complete symbol evidence requirement.
printf 'not a symbol table' >"$work/fake-symbol-data"
objcopy --add-section .symtab="$work/fake-symbol-data" "$work/stripped" "$work/spoofed-symtab"
readelf -W --symbols "$work/spoofed-symtab" >"$work/spoofed-readelf.stdout" 2>"$work/spoofed-readelf.stderr" || true
[[ -s "$work/spoofed-readelf.stderr" ]] || {
  echo 'spoofed ELF no longer emits readelf diagnostics; update negative oracle' >&2
  exit 1
}

"$clang" -O0 "$work/clean.c" -Wl,-rpath,"$work" -o "$work/runpath"
"$clang" -fPIC -shared "$work/foreign.c" -o "$work/libforeign.so"
"$clang" "$work/foreign_main.c" -L"$work" -lforeign -o "$work/foreign"
bash "$auditor" "$work/clean" >/dev/null
bash "$auditor" "$work/data" >/dev/null
for disallowed in collector collector2 stripped runpath foreign loader spoofed-symtab; do
  if bash "$auditor" "$work/$disallowed" >"$work/result.log" 2>&1; then
    echo "FAIL: unsafe/unverifiable binary admitted: $disallowed" >&2
    exit 1
  fi
  grep -Fq 'LINKED_GC_AUDIT_' "$work/result.log" || {
    cat "$work/result.log" >&2; echo "FAIL: missing rejection receipt: $disallowed" >&2; exit 1;
  }
done
printf 'LINKED_AOT_GC_SCREEN_TESTS_PASS|positive=2|negative=7|guest_executed=NO\n'
