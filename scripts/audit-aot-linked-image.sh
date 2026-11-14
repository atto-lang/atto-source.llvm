#!/usr/bin/env bash
# Linux ELF known-collector/linkage screen, NOT process-wide zero-GC proof.
# Audit before stripping the finished AOT executable.
set -euo pipefail
if [[ $# != 1 || ! -f "$1" ]]; then
  echo 'usage: audit-aot-linked-image.sh ELF_EXECUTABLE' >&2
  exit 64
fi
binary="$1"
for tool in readelf awk grep sed mktemp; do
  command -v "$tool" >/dev/null || {
    echo "LINKED_GC_AUDIT_UNAVAILABLE: $tool" >&2
    exit 69
  }
done
# GNU readelf can print "Error:" and return zero on malformed section tables.
# Both nonzero status AND any diagnostics must fail closed, even for metadata
# read-only operations. No execution of the untrusted ELF is performed.
diagnostics="$(mktemp)"
trap 'rm -f -- "$diagnostics"' EXIT
readelf_checked() {
  local result
  : > "$diagnostics"
  if ! result="$(readelf -W "$@" "$binary" 2>"$diagnostics")"; then
    echo 'LINKED_GC_AUDIT_UNVERIFIABLE: readelf failed' >&2
    cat "$diagnostics" >&2
    return 65
  fi
  if [[ -s "$diagnostics" ]]; then
    echo 'LINKED_GC_AUDIT_UNVERIFIABLE: readelf emitted diagnostics' >&2
    cat "$diagnostics" >&2
    return 65
  fi
  printf '%s\n' "$result"
}
readelf_checked -h >/dev/null || exit 65
sections="$(readelf_checked -S)" || exit 65
# A named .symtab section is not sufficient: a faked .symtab may contain
# arbitrary data or zero entries. Verify its ELF SHT_SYMTAB section type.
if ! grep -Eq '[[:space:]]\.symtab[[:space:]]+SYMTAB[[:space:]]' <<<"$sections"; then
  echo 'LINKED_GC_AUDIT_UNVERIFIABLE: no valid SHT_SYMTAB section' >&2
  exit 65
fi
dynamic="$(readelf_checked -d)" || exit 65
if grep -Eq '\((RPATH|RUNPATH|AUDIT|DEPAUDIT|TEXTREL|FILTER|AUXILIARY)\)' <<<"$dynamic"; then
  echo 'LINKED_GC_AUDIT_UNVERIFIABLE: dynamic search, injection or text relocation' >&2
  exit 65
fi
while IFS= read -r needed; do
  [[ -n "$needed" ]] || continue
  case "$needed" in
    libc.so.*|libm.so.*|libpthread.so.*|libgcc_s.so.*|libdl.so.*|librt.so.*|libatomic.so.*) ;;
    *) echo "LINKED_GC_AUDIT_UNKNOWN_DEPENDENCY: $needed" >&2; exit 65 ;;
  esac
done < <(sed -n '/(NEEDED)/s/^.*\[\([^]]*\)\].*$/\1/p' <<<"$dynamic")
# Never run ldd on an artifact being checked: it can invoke an ELF loader.
symbols="$(readelf_checked --symbols)" || exit 65
# A manipulated ELF can include a SHT_SYMTAB containing zero entries; readelf
# may still print a successful-looking symbol listing from .dynsym.
if ! grep -Eq "^Symbol table '\.symtab' contains ([2-9]|[1-9][0-9]+) entries:" <<<"$symbols"; then
  echo 'LINKED_GC_AUDIT_UNVERIFIABLE: missing populated static symbol table' >&2
  exit 65
fi
if ! awk '
  /^[[:space:]]*[0-9]+:/ && NF >= 8 {
    symbol=$8;
    sub(/@.*/, "", symbol);
    sub(/^_+/, "", symbol);
    if (symbol ~ /^(GC_malloc|GC_realloc|GC_gcollect|GC_enable|GC_disable|mono_gc_|g_gc_|jl_gc_collect|PyGC_Collect|gc_collect|gc_alloc|gc_mark|gc_sweep|gc_root|dlopen|dlmopen|dlsym|dlvsym|libc_dlopen_mode|LLVMOrc)/) {
      print "LINKED_GC_AUDIT_REJECTED_SYMBOL: " symbol > "/dev/stderr";
      found=1;
    }
  }
  END { exit (found ? 1 : 0) }
' <<<"$symbols"; then
  exit 65
fi
echo 'LINKED_AOT_KNOWN_GC_SYMBOLS_SCREENED_ONLY (NOT process zero-GC certification)'
