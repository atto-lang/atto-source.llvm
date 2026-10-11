#include "oreslang/runtime/SourceActorBootstrap.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic int active;
static _Atomic int calls[4];
static uint8_t expected[4][512];
static size_t lengths[4];

static uint8_t digit(char h) {
    if (h >= '0' && h <= '9') return (uint8_t)(h - '0');
    if (h >= 'a' && h <= 'f') return (uint8_t)(h - 'a' + 10);
    abort();
}
static size_t from_hex(const char* hex, uint8_t* out) {
    size_t length = strlen(hex);
    assert(length % 2 == 0 && length / 2 <= 512);
    for (size_t i = 0; i < length / 2; ++i)
        out[i] = (uint8_t)((digit(hex[2*i]) << 4) | digit(hex[2*i+1]));
    return length / 2;
}
static void actor_init(OresActorI64Heap* heap, uint64_t d, uint64_t e) {
    ores_actor_i64_heap_field_init_mutable(heap, d, e, 0, 0);
}
static int64_t receive_variant(OresActorI64Heap* heap, uint64_t d,
                               uint64_t e, size_t variant,
                               const uint8_t* bytes, size_t size) {
    assert(atomic_fetch_add(&active, 1) == 0);
    assert(variant < 4 && size == lengths[variant]);
    assert(memcmp(bytes, expected[variant], size) == 0);
    int64_t n = ores_actor_i64_heap_field_read(heap, d, e, 0);
    ores_actor_i64_heap_field_write(heap, d, e, 0, n + 1);
    atomic_fetch_add(&calls[variant], 1);
    assert(atomic_fetch_sub(&active, 1) == 1);
    return n + 1;
}
int main(void) {
    lengths[0] = from_hex(
        "9402ae64656d6f2e537472696e672e7631c420"
        "21c2efdbff08dd4e1320d8abe1ba0cc6aea8e4cf104b984be7b7079968fad3cda3666f6f",
        expected[0]);
    lengths[1] = lengths[0];
    memcpy(expected[1], expected[0], lengths[0]);
    /* Both string alternatives have the same payload/digest but distinct
     * compiler-pinned nominal schema identity, not a guessed string type. */
    expected[1][3 + strlen("demo.String.v1") - 1] = '2';
    lengths[2] = from_hex(
        "9402b764656d6f2e5475706c65426f6f6c537472696e672e7631"
        "c420d254b4ecd3d7964454dcdf86fd10ede585a17b3af27982a289819afdc2379ab292c3a3666f6f",
        expected[2]);
    lengths[3] = from_hex(
        "9402d98f6f7265736c616e672e656e756d2e3634613738626261303435333935353237393063366335376665613066303533"
        "37346338373864623836313134393130356261366631373332633837326539342e3462626434633231666330303764383963"
        "6136386637663061396136313438666162313266376539313361353931636633626131383033333061353931383634c42008"
        "5917a950bf1470f59d654358e5da6f1744d6eadd4c4128f334ddbb6e032ddb93a653746174757302a568656c6c6f",
        expected[3]);
    char schema_0[] = "demo.String.v1";
    char schema_1[] = "demo.String.v2";
    char schema_2[] = "demo.TupleBoolString.v1";
    char schema_3[] = "oreslang.enum.64a78bba04539552790c6c57fea0f05374c878db861149105ba6f1732c872e94.4bbd4c21fc007d89ca68f7f0a9a6148fab12f7e913a591cf3ba180330a591864";
    char shape_0[] = "S";
    char shape_1[] = "S";
    char shape_2[] = "T[B,S]";
    char shape_3[] = "T[S,I,S]";
    uint8_t digest[4][32];
    const size_t schema_len[4] = {strlen(schema_0), strlen(schema_1),
                                   strlen(schema_2), strlen(schema_3)};
    for (size_t i = 0; i < 4; ++i) {
        const size_t index = 3 + (schema_len[i] > 31 ? 1 : 0) + schema_len[i];
        assert(expected[i][index] == 0xc4 && expected[i][index+1] == 32);
        memcpy(digest[i], expected[i] + index + 2, 32);
    }
    OresMp2Contract contracts[4] = {
        {schema_0, shape_0, digest[0]},
        {schema_1, shape_1, digest[1]},
        {schema_2, shape_2, digest[2]},
        {schema_3, shape_3, digest[3]}
    };
    for (size_t i = 0; i < 4; ++i) assert(ores_mp2_contract_valid(&contracts[i]));
    assert(!oreslang_source_actor_spawn_variants(
        actor_init, receive_variant, 1, contracts, 0));
    assert(!oreslang_source_actor_spawn_variants(
        actor_init, receive_variant, 1, contracts, 65));
    OresMp2Contract duplicate[2] = {contracts[0], contracts[0]};
    assert(!oreslang_source_actor_spawn_variants(
        actor_init, receive_variant, 1, duplicate, 2));
    OresMp2Contract forged[2] = {contracts[0], contracts[1]};
    uint8_t wrong_sha[32];
    memcpy(wrong_sha, digest[1], 32);
    wrong_sha[0] ^= 1;
    forged[1].shape_sha256 = wrong_sha;
    assert(!oreslang_source_actor_spawn_variants(
        actor_init, receive_variant, 1, forged, 2));
    void* actor = oreslang_source_actor_spawn_variants(
        actor_init, receive_variant, 1, contracts, 4);
    assert(actor);
    /* Guest immutable compiler descriptors have been completely copied. */
    memset(schema_0, 'z', strlen(schema_0));
    memset(schema_1, 'z', strlen(schema_1));
    memset(schema_2, 'z', strlen(schema_2));
    memset(schema_3, 'z', strlen(schema_3));
    shape_0[0] = 'I';
    shape_1[0] = 'I';
    shape_2[0] = 'I';
    shape_3[0] = 'I';
    memset(digest, 0, sizeof(digest));
    uint8_t wrong[512];
    memcpy(wrong, expected[0], lengths[0]);
    wrong[1] = 1;
    assert(!oreslang_source_actor_send_variant(actor, wrong, lengths[0]));
    memcpy(wrong, expected[0], lengths[0]);
    wrong[3] = 'X'; /* unknown schema, not a new dynamic decoder */
    assert(!oreslang_source_actor_send_variant(actor, wrong, lengths[0]));
    memcpy(wrong, expected[0], lengths[0]);
    wrong[3 + strlen("demo.String.v1") + 2] ^= 1;
    assert(!oreslang_source_actor_send_variant(actor, wrong, lengths[0]));
    for (size_t i = 0; i < 4; ++i) {
        uint8_t sender[512];
        memcpy(sender, expected[i], lengths[i]);
        assert(oreslang_source_actor_send_variant(actor, sender, lengths[i]));
        memset(sender, 0, lengths[i]);
    }
    assert(!oreslang_source_actor_send_wire(actor, expected[0], lengths[0]));
    oreslang_source_actor_stop(actor);
    for (size_t i = 0; i < 4; ++i) assert(atomic_load(&calls[i]) == 1);
    assert(atomic_load(&active) == 0);
    puts("SOURCE_ACTOR_CLOSED_VARIANTS_PASS same_lane=1 nominal_tags=1 single_budget=1 java_tagged_wire=1");
    return 0;
}
