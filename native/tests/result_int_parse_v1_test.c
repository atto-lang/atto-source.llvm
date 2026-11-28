#include "atto/result_int_parse_v1.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static atto_result_i64_v1 parse(atto_parse_scope_v1 *s, const char *p, size_t n) {
    atto_result_i64_v1 out;
    assert(atto_parse_i64_result_v1(s, (const unsigned char *)p, n, &out) == 0);
    return out;
}
static atto_result_i64_v1 text(atto_parse_scope_v1 *s, const char *p) {
    return parse(s, p, strlen(p));
}
int main(void) {
    atto_parse_scope_v1 a, b;
    assert(atto_parse_scope_init_v1(&a, 101) == 0);
    assert(atto_parse_scope_init_v1(&b, 102) == 0);
    assert(atto_parse_scope_init_v1(NULL, 101) == -1);
    assert(atto_parse_scope_init_v1(&b, 0) == -1);
    assert(atto_parse_scope_init_v1(&b, 102) == 0);
    char buf[64];
    for (int64_t n = -20000; n <= 20000; ++n) {
        (void)snprintf(buf, sizeof(buf), "%" PRId64, n);
        atto_result_i64_v1 r = text(&a, buf);
        assert(r.tag == ATTO_RESULT_OK_V1 && r.value == n);
    }
    assert(text(&a, "-9223372036854775808").value == INT64_MIN);
    assert(text(&a, "9223372036854775807").value == INT64_MAX);
    assert(text(&a, "+42").value == 42);
    assert(text(&a, "00042").value == 42);
    assert(text(&a, "-0").value == 0);
    const char *invalid[] = {
        "", "+", "-", " 42", "42 ", "4_2", "++1",
        "9223372036854775808", "-9223372036854775809",
        "42x", "4.2", "\xd9\xa4\xd9\xa2", "0x2a"
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        const atto_result_i64_v1 r = text(&a, invalid[i]);
        assert(r.tag == ATTO_RESULT_ERR_V1 &&
               r.error_code == ATTO_PARSE_INVALID_INTEGER_V1);
        assert(atto_parse_error_visible_v1(&a, &r));
        assert(!atto_parse_error_visible_v1(&b, &r));
    }
    const unsigned char embedded_nul[3] = {'4', 0, '2'};
    atto_result_i64_v1 r;
    assert(atto_parse_i64_result_v1(&a, embedded_nul, 3, &r) == 0);
    assert(r.tag == ATTO_RESULT_ERR_V1);
    char zeros[1025];
    memset(zeros, '0', sizeof(zeros));
    r = parse(&a, zeros, 1024);
    assert(r.tag == ATTO_RESULT_OK_V1 && r.value == 0);
    r = parse(&a, zeros, 1025);
    assert(r.tag == ATTO_RESULT_ERR_V1);
    const atto_result_i64_v1 first = text(&a, "bad");
    const atto_result_i64_v1 propagated = first;
    const atto_result_i64_v1 second = text(&a, "bad");
    assert(propagated.error_id == first.error_id);
    assert(second.error_id != first.error_id);
    assert(atto_parse_error_visible_v1(&a, &propagated));
    assert(!atto_parse_error_visible_v1(&b, &propagated));
    const atto_result_i64_v1 own = text(&b, "bad");
    assert(atto_parse_error_visible_v1(&b, &own));
    atto_result_i64_v1 out;
    assert(atto_parse_i64_result_v1(NULL, (const unsigned char *)"1", 1, &out) == -1);
    assert(atto_parse_i64_result_v1(&a, NULL, 0, &out) == -1);
    assert(atto_parse_i64_result_v1(&a, (const unsigned char *)"1", 1, NULL) == -1);
    puts("ATTO_LLVM_RESULT_HOST_PARSE_PASS");
}
