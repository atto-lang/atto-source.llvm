#ifndef ORESLANG_NATIVE_STRING_H
#define ORESLANG_NATIVE_STRING_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Proposed LLVM {i8*,i64} ABI on 64-bit hosts. Own and drop exactly once.
// Zero-initialize output before calls. NEVER copy an owning struct by value.
// The compiler does not yet enforce the ownership restriction.
typedef struct OreslangNativeString {
    unsigned char* data;
    uint64_t byte_length;
} OreslangNativeString;
typedef enum OreslangStringStatus {
    ORESLANG_STRING_OK = 0,
    ORESLANG_STRING_INVALID = 1,
    ORESLANG_STRING_INVALID_UTF8 = 2,
    ORESLANG_STRING_TOO_LARGE = 3,
    ORESLANG_STRING_NO_MEMORY = 4
} OreslangStringStatus;
#define ORESLANG_STRING_MAX_BYTES UINT64_C(16777216)
// Strings may contain NUL; byte_length is authoritative, not strlen.
// Input borrows must stay alive until the operation returns.
OreslangStringStatus oreslang_string_new_utf8(const unsigned char*, uint64_t, OreslangNativeString*);
OreslangStringStatus oreslang_string_clone(const OreslangNativeString*, OreslangNativeString*);
OreslangStringStatus oreslang_string_concat(const OreslangNativeString*, const OreslangNativeString*, OreslangNativeString*);
OreslangStringStatus oreslang_string_json_quote(const OreslangNativeString*, OreslangNativeString*);
void oreslang_string_drop(OreslangNativeString*);
#ifdef __cplusplus
}
#endif
#endif
