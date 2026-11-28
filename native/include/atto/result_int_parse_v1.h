#ifndef ATTO_RESULT_INT_PARSE_V1_H
#define ATTO_RESULT_INT_PARSE_V1_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* HOST-ONLY proof substrate. NOT an Atto guest ABI or LLVM compiler lowering.
 * No pointers/references to one TenantVM's heap cross the interface.
 * Error identity consists of both tenant_id and error_id. */
enum { ATTO_RESULT_OK_V1 = 0, ATTO_RESULT_ERR_V1 = 1 };
enum { ATTO_PARSE_ERROR_NONE_V1 = 0, ATTO_PARSE_INVALID_INTEGER_V1 = 1 };
enum { ATTO_RESULT_PARSE_MAX_BYTES_V1 = 1024 };

typedef struct atto_parse_scope_v1 {
    uint64_t tenant_id;
    uint64_t next_error_id;
} atto_parse_scope_v1;
typedef struct atto_result_i64_v1 {
    uint32_t tag;
    uint32_t error_code;
    int64_t value;
    uint64_t tenant_id;
    uint64_t error_id;
} atto_result_i64_v1;

/* 0: initialized; -1: denied tenant/scope */
int atto_parse_scope_init_v1(atto_parse_scope_v1 *scope, uint64_t tenant_id);

/* 0: valid tagged Ok or Err returned; -1: invalid host parameters;
 * -2: fatal Error-identity quota exhaustion. Untrusted numeric input never
 * causes a host exception; malformed/oversize returns Err in *out. */
int atto_parse_i64_result_v1(atto_parse_scope_v1 *scope,
                             const unsigned char *bytes, size_t len,
                             atto_result_i64_v1 *out);

/* A value may be observed only by its owning trusted tenant.
 * Same tagged record can be copied during ? propagation without rewrapping. */
int atto_parse_error_visible_v1(const atto_parse_scope_v1 *scope,
                                 const atto_result_i64_v1 *value);
#ifdef __cplusplus
}
#endif
#endif
