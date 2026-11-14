#!/usr/bin/env bash
# Linux ELF known-collector/linkage screen, NOT process-wide zero-GC proof.
# Audit before stripping the finished AOT executable.
set -euo pipefail
if [[ $# != 1 || ! -f "$1" ]]; then
  echo 'usage: audit-aot-linked-image.sh ELF_EXECUTABLE' >&2
  exit 64
fi
binary="$1"
for tool in readelf awk grep sed; do
  command -v "$tool" >/dev/null || { echo "LINKED_GC_AUDIT_UNAVAILABLE: $tool" >&2; exit 69; }
done
if ! readelf -Wh "$binary" >/dev/null 2>&1; then
  echo 'LINKED_GC_AUDIT_UNSUPPORTED_FORMAT: not ELF' >&2
  exit 65
fi
if ! readelf -W -S "$binary" | grep -Eq '[[:space:]\.symtab[[:space:]]'; then
  echo 'LINKED_GC_AUDIT_UNVERIFIABLE: .symtab stripped before audit' >&2
  exit 65
fi
# This is a narrow known-linkage screen. Libc itself is not a tracing GC,
# but an allowlisted dependency is NOT proof of all its transitive behavior.
dynamic="$(readelf -W -d "$binary")"
if printf '%s\n' "$dynamic" | grep -Eq '\((RPATH|RUNPATH|AUDIT|DEPAUDIT|TEXTREL)\)'; then
  echo 'LINKED_GC_AUDIT_UNVERIFIABLE: dynamic search/injection/text relocation' >&2
  exit 65
fi
while IFS= read -r needed; do
  [[ -n "$needed" ]] || continue
  case "$needed" in
    libc.so.*|libm.so.*|libpthread.so.*|libgcc_s.so.*|libdl.so.*|librt.so.*|libatomic.so.*) ;;
    *) echo "LINKED_GC_AUDIT_UNKNOWN_DEPENDENCY: $needed" >&2; exit 65;;
  esac
done < <(printf '%s\n' "$dynamic" | sed -n '/(NEEDED)/s/^.*\[\([^]]*\)\].*$/\1/p')
# Never run ldd on an artifact being checked: it can invoke an ELF loader.
symbols="$(readelf -W --symbols "$binary")"
if ! printf '%s\n' "$symbols" | awk '
  /^[[:space:]]*[0-9]+:/ && NF >= 8 {
    symbol=$8;
    sub(/@.*/, "", symbol);
    sub(/^_+/, "", symbol);
    if (symbol ~ /^(GC_malloc|GC_realloc|GC_gcollect|GC_enable|GC_disable|mono_gc_|g_gc_|jl_gc_collect|PyGC_Collect|gc_collect|gc_alloc|gc_mark|gc_sweep|gc_root)/) {
      print "LINKED_GC_AUDIT_REJECTED_SYMBOL: " symbol > "/dev/stderr";
      found=1;
    }
  }
  END { exit (found ? 1 : 0) }
'; then
  exit 65
fi
echo 'LINKED_AOT_KNOWN_GC_SYMBOLS_SCREENED_ONLY (NOT process zero-GC certification)'
