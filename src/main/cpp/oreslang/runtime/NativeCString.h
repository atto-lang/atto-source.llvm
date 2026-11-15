#ifndef ORESLANG_NATIVE_CSTRING_H
#define ORESLANG_NATIVE_CSTRING_H

#include "oreslang/runtime/NativeString.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Explicit FFI adapter for APIs that require NUL-terminated UTF-8.
 * byte_length excludes the terminator. data owns byte_length + 1 bytes and
 * data[byte_length] is NUL. Embedded NUL is forbidden in the payload.
 *
 * This type is intentionally distinct from OreslangNativeString. Ordinary
 * Oreslang strings remain explicit spans and may contain embedded NUL.
 */
typedef struct OreslangNativeCString {
    unsigned char* data;
    uint64_t byte_length;
} OreslangNativeCString;

OreslangStringStatus oreslang_cstring_from_string(
        const OreslangNativeString*, OreslangNativeCString*);
OreslangStringStatus oreslang_cstring_to_string(
        const OreslangNativeCString*, OreslangNativeString*);
void oreslang_cstring_drop(OreslangNativeCString*);

#ifdef __cplusplus
}
#endif
#endif
