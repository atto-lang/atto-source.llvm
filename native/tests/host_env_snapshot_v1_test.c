#include "atto/host_env_snapshot_v1.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    unsigned char original[] = "atto";
    const char empty[] = "";
    const char *allow[] = { "*" };
    const char *deny[] = { "PRIVATE_*" };
    atto_host_env_entry_v1 entries[] = {
        { "PUBLIC_NAME", original, 4 },
        { "PUBLIC_EMPTY", (const unsigned char *)empty, 0 },
        /* Host MUST NOT dereference excluded pointers, even with invalid size. */
        { "PRIVATE_SECRET", (const unsigned char *)(uintptr_t)1, 16385 }
    };
    atto_host_env_snapshot_v1 *env = NULL;
    assert(atto_host_env_create_v1(entries, 3, allow, 1, deny, 1, true, &env) == ATTO_ENV_V1_OK);
    assert(env != NULL);
    original[0] = 'X'; /* proof of copied storage */
    unsigned char buf[8] = {0};
    size_t count = 0;
    bool present = false;
    assert(atto_host_env_get_v1(env, "PUBLIC_NAME", buf, sizeof buf, &count) == ATTO_ENV_V1_OK);
    assert(count == 4 && memcmp(buf, "atto", 4) == 0);
    assert(atto_host_env_has_v1(env, "PUBLIC_EMPTY", &present) == ATTO_ENV_V1_OK && present);
    assert(atto_host_env_get_v1(env, "PUBLIC_EMPTY", NULL, 0, &count) == ATTO_ENV_V1_OK && count == 0);
    assert(atto_host_env_get_v1(env, "PUBLIC_MISSING", NULL, 0, &count) == ATTO_ENV_V1_ABSENT);
    assert(atto_host_env_has_v1(env, "PUBLIC_MISSING", &present) == ATTO_ENV_V1_OK && !present);
    assert(atto_host_env_has_v1(env, "PRIVATE_SECRET", &present) == ATTO_ENV_V1_DENIED && !present);
    assert(atto_host_env_get_v1(env, "PRIVATE_SECRET", buf, sizeof buf, &count) == ATTO_ENV_V1_DENIED);
    assert(atto_host_env_get_v1(env, "PUBLIC_NAME", buf, 1, &count) == ATTO_ENV_V1_TOO_SMALL && count == 4);
    assert(atto_host_env_get_v1(env, "INVALID-NAME", buf, sizeof buf, &count) == ATTO_ENV_V1_INVALID);
    assert(atto_host_env_revoke_v1(env));
    assert(!atto_host_env_revoke_v1(env));
    assert(atto_host_env_get_v1(env, "PUBLIC_NAME", buf, sizeof buf, &count) == ATTO_ENV_V1_DENIED);
    atto_host_env_destroy_v1(env);

    env = NULL;
    assert(atto_host_env_create_v1(entries, 3, allow, 1, deny, 1, false, &env) == ATTO_ENV_V1_OK);
    assert(atto_host_env_get_v1(env, "PUBLIC_NAME", buf, sizeof buf, &count) == ATTO_ENV_V1_DENIED);
    atto_host_env_destroy_v1(env);

    // With coarse ENV absent, not even a name pointer may be touched.
    // A broken admission order would segfault/ASan-fail this case.
    atto_host_env_entry_v1 unreadable[] = {
        { (const char *)(uintptr_t)1, (const unsigned char *)(uintptr_t)1, 9 }
    };
    assert(atto_host_env_create_v1(unreadable, 1, allow, 1,
                                   deny, 1, false, &env) == ATTO_ENV_V1_OK);
    assert(env != NULL);
    assert(atto_host_env_has_v1(env, "PUBLIC_NAME", &present) == ATTO_ENV_V1_DENIED);
    atto_host_env_destroy_v1(env);
    env = NULL;

    const char *bad_rules[] = { "A**" };
    assert(atto_host_env_create_v1(entries, 3, bad_rules, 1, deny, 1, true, &env) == ATTO_ENV_V1_INVALID);
    assert(env == NULL);

    const unsigned char nul_value[] = {'a', 0, 'b'};
    atto_host_env_entry_v1 nul[] = { { "ATTO_NUL", nul_value, sizeof nul_value } };
    assert(atto_host_env_create_v1(nul, 1, allow, 1, deny, 1, true, &env) == ATTO_ENV_V1_INVALID);
    assert(env == NULL);

    const unsigned char invalid_utf8[] = {0xed, 0xa0, 0x80};
    atto_host_env_entry_v1 bad[] = { { "ATTO_BAD", invalid_utf8, sizeof invalid_utf8 } };
    assert(atto_host_env_create_v1(bad, 1, allow, 1, deny, 1, true, &env) == ATTO_ENV_V1_INVALID);
    assert(env == NULL);

    atto_host_env_entry_v1 duplicate[] = {
        { "ATTO_DUP", (const unsigned char *)"first", 5 },
        { "ATTO_DUP", (const unsigned char *)"again", 5 }
    };
    assert(atto_host_env_create_v1(duplicate, 2, allow, 1, deny, 1, true, &env) == ATTO_ENV_V1_INVALID);
    assert(env == NULL);
    // Every value is individually legal but aggregate admission is bounded
    // to exactly 1 MiB. Denied values are never dereferenced or counted.
    unsigned char block[16384];
    memset(block, 'x', sizeof block);
    char budget_names[65][32];
    atto_host_env_entry_v1 budget[65];
    for (size_t i = 0; i < 65; ++i) {
        snprintf(budget_names[i], sizeof budget_names[i], "ATTO_BUDGET_%zu", i);
        budget[i] = (atto_host_env_entry_v1) {
            budget_names[i], block, sizeof block
        };
    }
    const char *budget_grants[] = { "ATTO_BUDGET_*" };
    assert(atto_host_env_create_v1(budget, 64, budget_grants, 1,
                                   NULL, 0, true, &env) == ATTO_ENV_V1_OK);
    assert(env != NULL);
    atto_host_env_destroy_v1(env);
    env = NULL;
    assert(atto_host_env_create_v1(budget, 65, budget_grants, 1,
                                   NULL, 0, true, &env) == ATTO_ENV_V1_INVALID);
    assert(env == NULL);
    budget[64].value = (const unsigned char *)(uintptr_t)1;
    budget[64].value_length = (size_t)-1;
    const char *budget_denies[] = { "ATTO_BUDGET_64" };
    assert(atto_host_env_create_v1(budget, 65, budget_grants, 1,
                                   budget_denies, 1, true, &env) == ATTO_ENV_V1_OK);
    assert(env != NULL);
    atto_host_env_destroy_v1(env);
    env = NULL;

    return 0;
}
