#!/usr/bin/env bash
# Source-free native sanitizer controls: this tests HOST instrumentation only.
set -euo pipefail
if (( $# < 1 )); then
  echo 'usage: verify-native-sanitizer-controls.sh address|undefined|thread ...' >&2
  exit 2
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for sanitizer in "$@"; do
  case "$sanitizer" in
    address)
      compiler="${SANITIZER_CC:-clang}"
      cat >"$tmp/probe.c" <<'C'
#include <stdlib.h>
int main(void) {
    char *p = (char *)malloc(8);
    if (!p) return 3;
    volatile char *v = p;
    v[8] = 42;
    free(p);
    return 0;
}
C
      diagnostic='ERROR: AddressSanitizer:'
      ;;
    undefined)
      compiler="${SANITIZER_CC:-clang}"
      cat >"$tmp/probe.c" <<'C'
#include <limits.h>
#include <stdio.h>
int main(void) {
    volatile int value = INT_MAX;
    printf("%d\n", value + 1);
    return 0;
}
C
      diagnostic='runtime error: signed integer overflow'
      ;;
    thread)
      compiler="${SANITIZER_CC:-gcc}"
      cat >"$tmp/probe.c" <<'C'
#define _GNU_SOURCE
#include <pthread.h>
static volatile int value = 0;
static pthread_barrier_t barrier;
static void *worker(void *ignored) {
    (void)ignored;
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < 10000; ++i) value++;
    return NULL;
}
int main(void) {
    pthread_t a, b;
    if (pthread_barrier_init(&barrier, NULL, 2)) return 3;
    if (pthread_create(&a, NULL, worker, NULL)) return 3;
    if (pthread_create(&b, NULL, worker, NULL)) return 3;
    if (pthread_join(a, NULL) || pthread_join(b, NULL)) return 3;
    pthread_barrier_destroy(&barrier);
    return 0;
}
C
      diagnostic='WARNING: ThreadSanitizer: data race'
      ;;
    *)
      printf 'Unknown sanitizer control: %s\n' "$sanitizer" >&2
      exit 2
      ;;
  esac
  command -v "$compiler" >/dev/null || {
    printf 'SANITIZER_CONTROL_INCONCLUSIVE kind=%s missing compiler=%s\n' "$sanitizer" "$compiler" >&2
    exit 2
  }
  # Require instrumentation at compile AND link time.
  "$compiler" -std=c11 -O1 -g -pthread -fno-omit-frame-pointer \
    -fno-sanitize-recover=all "-fsanitize=$sanitizer" \
    "$tmp/probe.c" -o "$tmp/probe"
  set +e
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:exitcode=86 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86 \
  TSAN_OPTIONS=halt_on_error=1:exitcode=86 \
    timeout 20s "$tmp/probe" >"$tmp/out" 2>"$tmp/err"
  status=$?
  set -e
  if (( status == 0 || status == 124 )) || ! grep -Fq "$diagnostic" "$tmp/err"; then
    printf 'SANITIZER_CONTROL_INCONCLUSIVE kind=%s compiler=%s exit=%s; expected %s\n' \
      "$sanitizer" "$compiler" "$status" "$diagnostic" >&2
    sed -n '1,16p' "$tmp/err" >&2
    exit 2
  fi
  if grep -Fq 'FATAL: ThreadSanitizer:' "$tmp/err"; then
    printf 'SANITIZER_CONTROL_INCONCLUSIVE kind=%s TSan startup failed\n' "$sanitizer" >&2
    exit 2
  fi
  printf 'SANITIZER_CONTROL_DETECTED kind=%s compiler=%s expected_race_or_ub=yes\n' \
    "$sanitizer" "$compiler"
done
