#include "oreslang/runtime/NativeCString.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int empty_cstring(const OreslangNativeCString* value) {
    return value && !value->data && !value->byte_length;
}

static OreslangStringStatus validate_cstring(const OreslangNativeCString* value) {
    if (!value || !value->data) return ORESLANG_STRING_INVALID;
    if (value->byte_length > ORESLANG_STRING_MAX_BYTES
            || value->byte_length > (uint64_t)(SIZE_MAX - 1)) {
        return ORESLANG_STRING_TOO_LARGE;
    }
    if (value->data[value->byte_length] != 0) return ORESLANG_STRING_INVALID;
    if (value->byte_length
            && memchr(value->data, 0, (size_t)value->byte_length) != NULL) {
        return ORESLANG_STRING_EMBEDDED_NUL;
    }
    OreslangNativeString span = {value->data, value->byte_length};
    return oreslang_string_validate(&span);
}

OreslangStringStatus oreslang_cstring_from_string(
        const OreslangNativeString* source, OreslangNativeCString* out) {
    if (!empty_cstring(out)) return ORESLANG_STRING_INVALID;
    OreslangStringStatus status = oreslang_string_validate(source);
    if (status != ORESLANG_STRING_OK) return status;
    if (source->byte_length
            && memchr(source->data, 0, (size_t)source->byte_length) != NULL) {
        return ORESLANG_STRING_EMBEDDED_NUL;
    }
    if (source->byte_length > (uint64_t)(SIZE_MAX - 1)) {
        return ORESLANG_STRING_TOO_LARGE;
    }
    size_t allocation = (size_t)source->byte_length + 1;
    unsigned char* bytes = (unsigned char*)malloc(allocation);
    if (!bytes) return ORESLANG_STRING_NO_MEMORY;
    if (source->byte_length) {
        memcpy(bytes, source->data, (size_t)source->byte_length);
    }
    bytes[source->byte_length] = 0;
    out->data = bytes;
    out->byte_length = source->byte_length;
    return ORESLANG_STRING_OK;
}

OreslangStringStatus oreslang_cstring_to_string(
        const OreslangNativeCString* source, OreslangNativeString* out) {
    OreslangStringStatus status = validate_cstring(source);
    if (status != ORESLANG_STRING_OK) return status;
    return oreslang_string_new_utf8(source->data, source->byte_length, out);
}

void oreslang_cstring_drop(OreslangNativeCString* value) {
    if (!value) return;
    free(value->data);
    value->data = NULL;
    value->byte_length = 0;
}
