/*
 * Bounded host stdin adapter for ONE real compiled AttoLang scalar function.
 * This is not a native AttoLang stdin/IO implementation or actor instrumentation.
 * Input bytes reach guest LLVM-generated fuzz_byte(i64) through the native ABI.
 * Exit 0: observed scalar property held; 77: reproducible guest discrepancy.
 * Exit 2: harness inconclusive/resource or IO failure. No success receipts.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern int64_t fuzz_byte(int64_t value);

enum { CHUNK = 4096, MAX_BYTES = 65536 };

int main(void) {
    unsigned char bytes[CHUNK];
    size_t total = 0;
    for (;;) {
        const size_t count = fread(bytes, 1, sizeof bytes, stdin);
        if (count > MAX_BYTES - total) {
            (void)fputs("ATTO_LLVM_FUZZ_INCONCLUSIVE input budget exceeded\n", stderr);
            return 2;
        }
        for (size_t i = 0; i < count; ++i) {
            const int64_t observed = fuzz_byte((int64_t)bytes[i]);
            const int64_t expected = (int64_t)bytes[i] + 1;
            if (observed != expected) {
                (void)fprintf(stderr,
                    "ATTO_LLVM_FUZZ_MISMATCH position=%zu input=%u expected=%" PRId64 " got=%" PRId64 "\n",
                    total + i, (unsigned)bytes[i], expected, observed);
                return 77;
            }
        }
        total += count;
        if (count < sizeof bytes) {
            if (ferror(stdin)) {
                (void)fputs("ATTO_LLVM_FUZZ_INCONCLUSIVE read error\n", stderr);
                return 2;
            }
            if (feof(stdin)) break;
        }
    }
    return 0;
}
