#include "atto/result_int_parse_v1.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

int atto_parse_scope_init_v1(atto_parse_scope_v1 *scope, uint64_t tenant_id) {
    if (!scope || tenant_id == 0) return -1;
    scope->tenant_id = tenant_id;
    scope->next_error_id = 0;
    return 0;
}

static int invalid(atto_parse_scope_v1 *scope, atto_result_i64_v1 *out) {
    if (scope->next_error_id == UINT64_MAX) return -2;
    out->tag = ATTO_RESULT_ERR_V1;
    out->error_code = ATTO_PARSE_INVALID_INTEGER_V1;
    out->tenant_id = scope->tenant_id;
    out->error_id = ++scope->next_error_id;
    return 0;
}
int atto_parse_i64_result_v1(atto_parse_scope_v1 *scope,
                             const unsigned char *bytes, size_t len,
                             atto_result_i64_v1 *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!scope || scope->tenant_id == 0 || !bytes) return -1;
    if (len == 0 || len > ATTO_RESULT_PARSE_MAX_BYTES_V1)
        return invalid(scope, out);

    size_t at = 0;
    int negative = 0;
    if (bytes[0] == (unsigned char)'-' || bytes[0] == (unsigned char)'+') {
        negative = bytes[0] == (unsigned char)'-';
        at = 1;
    }
    if (at == len) return invalid(scope, out);
    const int64_t limit = negative ? INT64_MIN : -INT64_MAX;
    const int64_t cutoff = limit / INT64_C(10);
    int64_t value = 0;
    for (; at < len; ++at) {
        const unsigned char c = bytes[at];
        if (c < (unsigned char)'0' || c > (unsigned char)'9')
            return invalid(scope, out);
        const int64_t digit = (int64_t)(c - (unsigned char)'0');
        if (value < cutoff) return invalid(scope, out);
        value *= INT64_C(10);
        if (value < limit + digit) return invalid(scope, out);
        value -= digit;
    }
    out->tag = ATTO_RESULT_OK_V1;
    out->value = negative ? value : -value;
    return 0;
}

int atto_parse_error_visible_v1(const atto_parse_scope_v1 *scope,
                                 const atto_result_i64_v1 *value) {
    return scope && value &&
           value->tag == ATTO_RESULT_ERR_V1 &&
           scope->tenant_id != 0 &&
           scope->tenant_id == value->tenant_id &&
           value->error_id != 0;
}
