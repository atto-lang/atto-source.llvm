#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/MessagePackActorV2.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *schema, *shape, *hex; } Golden;
static const Golden golden[] = {
    {"demo.TupleBoolString.v1", "T[B,S]", "9402b764656d6f2e5475706c65426f6f6c537472696e672e7631c420d254b4ecd3d7964454dcdf86fd10ede585a17b3af27982a289819afdc2379ab292c3a3666f6f"},
    {"demo.FixedArrayInts.v1", "FA[I,I,I]", "9402b664656d6f2e46697865644172726179496e74732e7631c4206a46f6c65c619cc1c5354f7bd3b35ec0fcfbc9866c0760a15b860cfef79773b993010203"},
    {"demo.FixedListNested.v1", "FL[L[S],B]", "9402b764656d6f2e46697865644c6973744e65737465642e7631c4202007a0279d88d3eca7d21cf32b37327b996593c1353963dd78555f56f1ba6ddc9292a161a162c2"},
    {"demo.ArrayStrings.v1", "A[S]", "9402b464656d6f2e4172726179537472696e67732e7631c420139722a10b9ef4d4af0cea1bfbc50ff92a0459aabf837f63a6fec052fe5dc5c992a161a162"},
    {"demo.ListStrings.v1", "L[S]", "9402b364656d6f2e4c697374537472696e67732e7631c4204efbea59591d5c31d9f13ee93ef5c1d954e653144d380bb2d064ea4fa6d56b8c92a161a162"},
    {"demo.String.v1", "S", "9402ae64656d6f2e537472696e672e7631c42021c2efdbff08dd4e1320d8abe1ba0cc6aea8e4cf104b984be7b7079968fad3cda3666f6f"}
};
static uint8_t read_hex_digit(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    abort();
}
static size_t decode_hex(const char* s, uint8_t* into, size_t cap) {
    size_t n = strlen(s);
    assert(n % 2 == 0 && n / 2 <= cap);
    for (size_t i = 0; i < n / 2; ++i)
        into[i] = (uint8_t)((read_hex_digit(s[i * 2]) << 4) | read_hex_digit(s[i * 2 + 1]));
    return n / 2;
}
static OresMp2Contract contract(size_t index, const uint8_t* bytes, size_t n) {
    /* The golden manifest's schema ID is followed immediately by c4 20
     * <sha256(shape)>; the SHA is pinned by compiler-generated declaration.
     */
    size_t schema = strlen(golden[index].schema);
    size_t at = 2 + 1 + schema;
    assert(n >= at + 34 && bytes[0] == 0x94 && bytes[1] == 2
        && bytes[2] == (uint8_t)(0xa0 | schema));
    assert(bytes[at] == 0xc4 && bytes[at + 1] == 32);
    return (OresMp2Contract){golden[index].schema, golden[index].shape, bytes + at + 2};
}
typedef struct {
    atomic_int entered[2], finished[2], release;
    atomic_int active[2], max_active[2];
    uint8_t expected[2][1024];
    size_t expected_size[2];
} State;
static void on_message(void* raw, size_t slot, const uint8_t* bytes, size_t n) {
    State* state = raw;
    assert(slot < 2);
    int active = atomic_fetch_add(&state->active[slot], 1) + 1;
    int old = atomic_load(&state->max_active[slot]);
    while (active > old && !atomic_compare_exchange_weak(&state->max_active[slot], &old, active)) {}
    atomic_fetch_add(&state->entered[slot], 1);
    if (slot == 0) while (!atomic_load(&state->release)) sched_yield();
    assert(n == state->expected_size[slot]);
    assert(memcmp(bytes, state->expected[slot], n) == 0);
    atomic_fetch_sub(&state->active[slot], 1);
    atomic_fetch_add(&state->finished[slot], 1);
}
static void wait_atomic(atomic_int* counter, int expected) {
    for (int tries = 0; tries < 1000000; ++tries) {
        if (atomic_load(counter) >= expected) return;
        sched_yield();
    }
    abort();
}
int main(void) {
    uint8_t wire[6][1024]; size_t sizes[6];
    OresMp2Contract schemas[6];
    for (size_t i = 0; i < 6; ++i) {
        sizes[i] = decode_hex(golden[i].hex, wire[i], sizeof(wire[i]));
        schemas[i] = contract(i, wire[i], sizes[i]);
        OresMp2Owned owned = {0};
        assert(ores_mp2_validate_copy(&schemas[i], wire[i], sizes[i], &owned));
        assert(owned.size == sizes[i] && memcmp(owned.bytes, wire[i], sizes[i]) == 0);
        assert(!ores_mp2_validate_copy(&schemas[i], wire[i], sizes[i], &owned));
        wire[i][sizes[i] - 1] ^= 0xff; /* detach from mutable sender bytes */
        assert(memcmp(owned.bytes, wire[i], sizes[i]) != 0);
        wire[i][sizes[i] - 1] ^= 0xff;
        ores_mp2_owned_release(&owned);
        ores_mp2_owned_release(&owned);
        assert(!owned.bytes && !owned.size);
        assert(!ores_mp2_validate_copy(&schemas[i], wire[i], sizes[i] - 1, &owned));
        uint8_t corrupted[1024];
        memcpy(corrupted, wire[i], sizes[i]);
        corrupted[1] = 1; /* v1 is not accepted under v2 */
        assert(!ores_mp2_validate_copy(&schemas[i], corrupted, sizes[i], &owned));
        memcpy(corrupted, wire[i], sizes[i]);
        corrupted[3 + strlen(golden[i].schema) + 2] ^= 1; /* forged shape hash */
        assert(!ores_mp2_validate_copy(&schemas[i], corrupted, sizes[i], &owned));
        memcpy(corrupted, wire[i], sizes[i]);
        corrupted[sizes[i]] = 0; /* trailing value */
        assert(!ores_mp2_validate_copy(&schemas[i], corrupted, sizes[i] + 1, &owned));
        assert(!ores_mp2_validate_copy(&schemas[(i + 1) % 6], wire[i], sizes[i], &owned));
    }
    State state = {0};
    state.expected_size[0] = sizes[0]; state.expected_size[1] = sizes[5];
    memcpy(state.expected[0], wire[0], sizes[0]);
    memcpy(state.expected[1], wire[5], sizes[5]);
    OresMp2ActorGroup* group = ores_mp2_group_open(2, 1, 2, on_message, &state);
    assert(group);
    assert(ores_mp2_group_contract(group, 0, schemas[0]));
    assert(!ores_mp2_group_contract(group, 0, schemas[0]));
    assert(ores_mp2_group_contract(group, 1, schemas[5]));
    OresMp2ActorRef a = ores_mp2_group_spawn(group, 0);
    OresMp2ActorRef b = ores_mp2_group_spawn(group, 1);
    assert(a.generation && b.generation);
    assert(!ores_mp2_group_contract(group, 0, schemas[0]));
    assert(!ores_mp2_group_spawn(group, 0).generation);
    assert(!ores_mp2_group_send(group, a, wire[5], sizes[5]));
    assert(!ores_mp2_group_send(group, b, wire[0], sizes[0]));
    assert(ores_mp2_group_send(group, a, wire[0], sizes[0]));
    wait_atomic(&state.entered[0], 1);
    /* Capacity counts an actively executing turn: cannot acquire a second
     * buffer/admission in the same lane. Independent actor may still run. */
    assert(!ores_mp2_group_send(group, a, wire[0], sizes[0]));
    assert(ores_mp2_group_send(group, b, wire[5], sizes[5]));
    wait_atomic(&state.finished[1], 1);
    wire[0][sizes[0] - 1] ^= 0xff; /* mutate sender after acceptance */
    atomic_store(&state.release, 1);
    assert(ores_mp2_group_stop(group, a));
    assert(ores_mp2_group_stop(group, b));
    assert(ores_mp2_group_wait_done(group, a));
    assert(ores_mp2_group_wait_done(group, b));
    assert(!ores_mp2_group_send(group, a, wire[0], sizes[0]));
    assert(ores_mp2_group_destroy(group));
    assert(atomic_load(&state.finished[0]) == 1);
    assert(atomic_load(&state.finished[1]) == 1);
    assert(atomic_load(&state.max_active[0]) == 1);
    assert(atomic_load(&state.max_active[1]) == 1);
    puts("MESSAGEPACK_V2_NATIVE_PASS vectors=6 owned=1 scheduler=1 overload=1 independent=1");
    return 0;
}
