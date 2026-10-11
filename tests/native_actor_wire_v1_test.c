#include "oreslang/runtime/ActorMessageWireV1.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Java ActorMessageWireV1Test asserts the same exact 66-byte frame. */
static const char* GOLDEN =
    "4f4d5347010000000102030405060708112233440000002a"
    "040000000403000000026869020000000000000007"
    "050000000201010100060000000103000000026f6b";
static const uint64_t SCHEMA = UINT64_C(0x0102030405060708);
static const uint32_t VARIANT = UINT32_C(0x11223344);
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    abort();
}
int main(void) {
    const size_t bytes = strlen(GOLDEN) / 2;
    assert(bytes == 66 && strlen(GOLDEN) == bytes * 2);
    uint8_t canonical[66];
    for (size_t i = 0; i < bytes; ++i) {
        canonical[i] = (uint8_t)((hexval(GOLDEN[2 * i]) << 4)
                              | hexval(GOLDEN[2 * i + 1]));
    }
    OresActorWireV1Frame packed = {0};
    assert(ores_actor_wire_v1_pack(SCHEMA, VARIANT,
            canonical + ORES_ACTOR_WIRE_V1_HEADER_SIZE,
            bytes - ORES_ACTOR_WIRE_V1_HEADER_SIZE, &packed));
    assert(packed.size == bytes);
    assert(memcmp(packed.bytes, canonical, bytes) == 0);

    OresActorWireV1Frame received = {0};
    assert(ores_actor_wire_v1_unpack(canonical, bytes,
            SCHEMA, VARIANT, &received));
    assert(received.size == bytes && memcmp(received.bytes, canonical, bytes) == 0);
    canonical[34] = 'X';
    assert(received.bytes[34] == 'h');
    assert(packed.bytes[34] == 'h');

    assert(!ores_actor_wire_v1_unpack(canonical, bytes, SCHEMA + 1, VARIANT,
                                      &(OresActorWireV1Frame){0}));
    assert(!ores_actor_wire_v1_unpack(canonical, bytes, SCHEMA, VARIANT + 1,
                                      &(OresActorWireV1Frame){0}));
    assert(!ores_actor_wire_v1_unpack(canonical, bytes - 1, SCHEMA, VARIANT,
                                      &(OresActorWireV1Frame){0}));
    assert(!ores_actor_wire_v1_pack(0, VARIANT, canonical + 24, 42,
                                    &(OresActorWireV1Frame){0}));
    assert(!ores_actor_wire_v1_pack(SCHEMA, 0, canonical + 24, 42,
                                    &(OresActorWireV1Frame){0}));
    canonical[34] = 'h';

    uint8_t invalid[66];
    memcpy(invalid, canonical, bytes);
    invalid[51] = 2; /* bool must be exactly 0 or 1 */
    assert(!ores_actor_wire_v1_unpack(invalid, bytes, SCHEMA, VARIANT,
                                      &(OresActorWireV1Frame){0}));
    memcpy(invalid, canonical, bytes);
    invalid[34] = 0xc0; invalid[35] = 0x80; /* overlong UTF-8 */
    assert(!ores_actor_wire_v1_unpack(invalid, bytes, SCHEMA, VARIANT,
                                      &(OresActorWireV1Frame){0}));
    memcpy(invalid, canonical, bytes);
    invalid[24] = 99; /* invalid node tag */
    assert(!ores_actor_wire_v1_unpack(invalid, bytes, SCHEMA, VARIANT,
                                      &(OresActorWireV1Frame){0}));
    memcpy(invalid, canonical, bytes);
    invalid[25] = 0xff; /* impossibly large collection count */
    assert(!ores_actor_wire_v1_unpack(invalid, bytes, SCHEMA, VARIANT,
                                      &(OresActorWireV1Frame){0}));
    memcpy(invalid, canonical, bytes);
    invalid[5] = 1; /* unknown flags */
    assert(!ores_actor_wire_v1_unpack(invalid, bytes, SCHEMA, VARIANT,
                                      &(OresActorWireV1Frame){0}));
    memcpy(invalid, canonical, bytes);
    invalid[23] = 0x29; /* wrong payload size */
    assert(!ores_actor_wire_v1_unpack(invalid, bytes, SCHEMA, VARIANT,
                                      &(OresActorWireV1Frame){0}));

    ores_actor_wire_v1_release(&packed);
    ores_actor_wire_v1_release(&packed); /* idempotent */
    ores_actor_wire_v1_release(&received);
    assert(packed.bytes == NULL && packed.size == 0);
    assert(received.bytes == NULL && received.size == 0);
    puts("ACTOR_WIRE_V1_PASS canonical=66 owned=1 tags=bool,int64,string,tuple,array,list");
    return 0;
}
