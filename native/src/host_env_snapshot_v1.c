#include "atto/host_env_snapshot_v1.h"
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 512u
#define MAX_RULES 128u
#define MAX_VALUE 16384u

typedef struct {
    char *key;
    unsigned char *value;
    size_t length;
} stored_env_entry;
struct atto_host_env_snapshot_v1 {
    stored_env_entry *items;
    size_t length;
    char **allowed;
    size_t allow_count;
    char **denied;
    size_t deny_count;
    bool coarse;
    bool revoked;
};

/* Bounded identifier/rule admission without scanning unbounded strings. */
static size_t bounded_length(const char *p) {
    if (!p) return 0;
    size_t n = 0;
    while (n <= 129u && p[n]) ++n;
    return n;
}
static bool valid_key(const char *name) {
    size_t n = bounded_length(name);
    if (n == 0 || n > 128) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char) name[i];
        bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        bool digit = (c >= '0' && c <= '9');
        if (!alpha && c != '_' && (i == 0 || !digit)) return false;
    }
    return true;
}
static bool valid_rule(const char *s) {
    size_t n = bounded_length(s);
    if (n == 1 && s[0] == '*') return true;
    if (valid_key(s)) return true;
    if (n < 2 || n > 129 || s[n - 1] != '*') return false;
    /* A key-like prefix followed by one trailing wildcard. */
    for (size_t i = 0; i + 1 < n; ++i) {
        unsigned char c = (unsigned char) s[i];
        bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        bool digit = (c >= '0' && c <= '9');
        if (!alpha && c != '_' && (i == 0 || !digit)) return false;
    }
    return true;
}
static char *copy_name(const char *src) {
    size_t len = strlen(src);
    char *p = malloc(len + 1u);
    if (p) memcpy(p, src, len + 1u);
    return p;
}
static bool matches_rule(const char *key, const char *rule) {
    if (rule[0] == '*' && rule[1] == '\0') return true;
    size_t n = strlen(rule);
    if (n && rule[n - 1] == '*')
        return strncmp(key, rule, n - 1u) == 0;
    return strcmp(key, rule) == 0;
}
static bool allowed_key(const atto_host_env_snapshot_v1 *env, const char *key) {
    bool matched = false;
    for (size_t i = 0; i < env->allow_count; ++i)
        if (matches_rule(key, env->allowed[i])) { matched = true; break; }
    if (!matched) return false;
    for (size_t i = 0; i < env->deny_count; ++i)
        if (matches_rule(key, env->denied[i])) return false;
    return true;
}
static bool valid_utf8(const unsigned char *s, size_t n) {
    if (!s && n) return false;
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++];
        // NUL bytes cannot exist in a process environment value.
        if (c == 0) return false;
        if (c < 0x80) continue;
        unsigned need = 0;
        unsigned lo = 0x80, hi = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) need = 1;
        else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            if (c == 0xe0) lo = 0xa0;
            if (c == 0xed) hi = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            if (c == 0xf0) lo = 0x90;
            if (c == 0xf4) hi = 0x8f;
        } else return false;
        if (need > n - i) return false;
        if (s[i] < lo || s[i] > hi) return false;
        ++i;
        for (unsigned k = 1; k < need; ++k, ++i)
            if (s[i] < 0x80 || s[i] > 0xbf) return false;
    }
    return true;
}
static void scrub(unsigned char *p, size_t n) {
    if (!p) return;
    volatile unsigned char *bytes = p;
    while (n--) *bytes++ = 0;
}
bool atto_host_env_revoke_v1(atto_host_env_snapshot_v1 *env) {
    if (!env || env->revoked) return false;
    env->revoked = true;
    for (size_t i = 0; i < env->length; ++i) {
        scrub(env->items[i].value, env->items[i].length);
        free(env->items[i].value);
        env->items[i].value = NULL;
        env->items[i].length = 0;
    }
    return true;
}
void atto_host_env_destroy_v1(atto_host_env_snapshot_v1 *env) {
    if (!env) return;
    (void) atto_host_env_revoke_v1(env);
    for (size_t i = 0; i < env->length; ++i) free(env->items[i].key);
    for (size_t i = 0; i < env->allow_count; ++i) free(env->allowed[i]);
    for (size_t i = 0; i < env->deny_count; ++i) free(env->denied[i]);
    free(env->items);
    free(env->allowed);
    free(env->denied);
    free(env);
}
static atto_host_env_status_v1 copy_rules(const char *const *input,
                                          size_t count, char ***out) {
    *out = NULL;
    if (count > MAX_RULES || (count && !input)) return ATTO_ENV_V1_INVALID;
    if (!count) return ATTO_ENV_V1_OK;
    char **copy = calloc(count, sizeof(*copy));
    if (!copy) return ATTO_ENV_V1_NO_MEMORY;
    for (size_t i = 0; i < count; ++i) {
        if (!valid_rule(input[i])) {
            for (size_t j = 0; j < i; ++j) free(copy[j]);
            free(copy);
            return ATTO_ENV_V1_INVALID;
        }
        copy[i] = copy_name(input[i]);
        if (!copy[i]) {
            for (size_t j = 0; j < i; ++j) free(copy[j]);
            free(copy);
            return ATTO_ENV_V1_NO_MEMORY;
        }
    }
    *out = copy;
    return ATTO_ENV_V1_OK;
}
atto_host_env_status_v1 atto_host_env_create_v1(
    const atto_host_env_entry_v1 *entries, size_t entry_count,
    const char *const *allows, size_t allow_count,
    const char *const *denies, size_t deny_count,
    bool coarse_env_capability, atto_host_env_snapshot_v1 **out) {
    if (!out) return ATTO_ENV_V1_INVALID;
    *out = NULL;
    if (entry_count > MAX_KEYS || (entry_count && !entries))
        return ATTO_ENV_V1_INVALID;
    atto_host_env_snapshot_v1 *env = calloc(1, sizeof(*env));
    if (!env) return ATTO_ENV_V1_NO_MEMORY;
    env->coarse = coarse_env_capability;
    atto_host_env_status_v1 status =
        copy_rules(allows, allow_count, &env->allowed);
    if (status != ATTO_ENV_V1_OK) goto failed;
    env->allow_count = allow_count;
    status = copy_rules(denies, deny_count, &env->denied);
    if (status != ATTO_ENV_V1_OK) goto failed;
    env->deny_count = deny_count;
    if (entry_count) {
        env->items = calloc(entry_count, sizeof(*env->items));
        if (!env->items) { status = ATTO_ENV_V1_NO_MEMORY; goto failed; }
    }
    for (size_t i = 0; i < entry_count; ++i) {
        // No coarse capability: do not dereference a single host entry name,
        // even if the trusted snapshot points at inaccessible storage.
        if (!env->coarse) continue;
        const char *key = entries[i].name;
        if (!valid_key(key) || !allowed_key(env, key)) continue;
        if (entries[i].value_length > MAX_VALUE ||
            !valid_utf8(entries[i].value, entries[i].value_length)) {
            status = ATTO_ENV_V1_INVALID; goto failed;
        }
        for (size_t j = 0; j < env->length; ++j)
            if (!strcmp(env->items[j].key, key)) {
                status = ATTO_ENV_V1_INVALID; goto failed;
            }
        stored_env_entry *item = &env->items[env->length];
        item->key = copy_name(key);
        if (!item->key) { status = ATTO_ENV_V1_NO_MEMORY; goto failed; }
        item->value = malloc(entries[i].value_length + 1u);
        if (!item->value) { free(item->key); item->key = NULL;
                            status = ATTO_ENV_V1_NO_MEMORY; goto failed; }
        if (entries[i].value_length)
            memcpy(item->value, entries[i].value, entries[i].value_length);
        item->value[entries[i].value_length] = 0;
        item->length = entries[i].value_length;
        ++env->length;
    }
    *out = env;
    return ATTO_ENV_V1_OK;
failed:
    atto_host_env_destroy_v1(env);
    return status;
}
static atto_host_env_status_v1 admission(const atto_host_env_snapshot_v1 *env,
                                          const char *name) {
    if (!env || !valid_key(name)) return ATTO_ENV_V1_INVALID;
    if (env->revoked || !env->coarse || !allowed_key(env, name))
        return ATTO_ENV_V1_DENIED;
    return ATTO_ENV_V1_OK;
}
atto_host_env_status_v1 atto_host_env_has_v1(
    const atto_host_env_snapshot_v1 *env, const char *name, bool *present) {
    if (!present) return ATTO_ENV_V1_INVALID;
    *present = false;
    atto_host_env_status_v1 status = admission(env, name);
    if (status != ATTO_ENV_V1_OK) return status;
    for (size_t i = 0; i < env->length; ++i)
        if (!strcmp(name, env->items[i].key)) {
            *present = true; break;
        }
    return ATTO_ENV_V1_OK;
}
atto_host_env_status_v1 atto_host_env_get_v1(
    const atto_host_env_snapshot_v1 *env, const char *name,
    unsigned char *output, size_t output_capacity, size_t *received) {
    if (!received || (!output && output_capacity)) return ATTO_ENV_V1_INVALID;
    *received = 0;
    atto_host_env_status_v1 status = admission(env, name);
    if (status != ATTO_ENV_V1_OK) return status;
    for (size_t i = 0; i < env->length; ++i) {
        const stored_env_entry *item = &env->items[i];
        if (strcmp(item->key, name)) continue;
        *received = item->length;
        if (item->length > output_capacity || (item->length && !output))
            return ATTO_ENV_V1_TOO_SMALL;
        if (item->length) memcpy(output, item->value, item->length);
        return ATTO_ENV_V1_OK;
    }
    return ATTO_ENV_V1_ABSENT;
}
