/*
 * Native signed-i64 oracle for an unchanged AttoLang main() program.
 *
 * The LLVM source compiler emits an exported i64 guest entrypoint
 * __oreslang_guest_main and an OS i32 main wrapper. The CTest gate renames
 * only the native object-file wrapper symbol using objcopy, then links this
 * fixed, independently reviewed C entrypoint against the SAME compiler
 * generated object. In particular, no AttoLang expression is rewritten.
 *
 * The expectation arrives as a decimal argv string, not C source text.
 * A wrong full-width result exits 43; a correct result exits 42.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

_Static_assert(sizeof(int64_t) == 8, "native i64 oracle requires 64-bit signed int");

extern int64_t __oreslang_guest_main(void);

int main(int argc, char **argv) {
  if (argc != 2 || argv == NULL || argv[1] == NULL) {
    return 120;
  }
  char *end = NULL;
  errno = 0;
  const long long parsed = strtoll(argv[1], &end, 10);
  if (errno != 0 || end == argv[1] || end == NULL || *end != '\0') {
    return 121;
  }
  const int64_t actual = __oreslang_guest_main();
  return actual == (int64_t)parsed ? 42 : 43;
}
