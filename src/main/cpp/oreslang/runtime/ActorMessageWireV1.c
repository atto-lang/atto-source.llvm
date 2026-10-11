#include "oreslang/runtime/ActorMessageWireV1.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t* bytes;
    size_t size;
    size_t pos;
    size_t nodes;
} WireReader;

static int remaining(const WireReader* r, size_t required) {
    return required <= r->size - r->pos;
}
static uint32_t get32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t get64(const uint8_t* p) {
    return ((uint64_t)get32(p) << 32) | (uint64_t)get32(p + 4);
}
static void put32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}
static void put64(uint8_t* p, uint64_t value) {
    put32(p, (uint32_t)(value >> 32));
    put32(p + 4, (uint32_t)value);
}

/* Strict shortest-form UTF-8; prohibit surrogates and code points > U+10FFFF.
 * Reject malformed data before it becomes an owned actor message. */
static int valid_utf8(const uint8_t* p, size_t len) {
    for (size_t i = 0; i < len;) {
        uint8_t a = p[i++];
        if (a <= 0x7f) continue;
        unsigned extra = 0;
        uint8_t lower = 0x80, upper = 0xbf;
        if (a >= 0xc2 && a <= 0xdf) {
            extra = 1;
        } else if (a == 0xe0) {
            extra = 2; lower = 0xa0;
        } else if (a >= 0xe1 && a <= 0xec) {
            extra = 2;
        } else if (a == 0xed) {
            extra = 2; upper = 0x9f;
        } else if (a >= 0xee && a <= 0xef) {
            extra = 2;
        } else if (a == 0xf0) {
            extra = 3; lower = 0x90;
        } else if (a >= 0xf1 && a <= 0xf3) {
            extra = 3;
        } else if (a == 0xf4) {
            extra = 3; upper = 0x8f;
        } else {
            return 0;
        }
        if (len - i < extra || p[i] < lower || p[i] > upper) return 0;
        ++i;
        for (unsigned j = 1; j < extra; ++j, ++i) {
            if (p[i] < 0x80 || p[i] > 0xbf) return 0;
        }
    }
    return 1;
}
static int check_node(WireReader* r, unsigned depth) {
    if (depth >= ORES_ACTOR_WIRE_V1_MAX_DEPTH
            || ++r->nodes > ORES_ACTOR_WIRE_V1_MAX_NODES
            || !remaining(r, 1)) return 0;
    uint8_t tag = r->bytes[r->pos++];
    if (tag == ORES_ACTOR_WIRE_BOOL) {
        if (!remaining(r, 1) || r->bytes[r->pos] > 1) return 0;
        r->pos++;
        return 1;
    }
    if (tag == ORES_ACTOR_WIRE_INT64) {
        if (!remaining(r, 8)) return 0;
        r->pos += 8;
        return 1;
    }
    if (tag == ORES_ACTOR_WIRE_STRING) {
        if (!remaining(r, 4)) return 0;
        uint32_t length = get32(r->bytes + r->pos);
        r->pos += 4;
        if (!remaining(r, length)
                || !valid_utf8(r->bytes + r->pos, length)) return 0;
        r->pos += length;
        return 1;
    }
    if (tag == ORES_ACTOR_WIRE_TUPLE || tag == ORES_ACTOR_WIRE_ARRAY
            || tag == ORES_ACTOR_WIRE_LIST) {
        if (!remaining(r, 4)) return 0;
        uint32_t count = get32(r->bytes + r->pos);
        r->pos += 4;
        if (count > ORES_ACTOR_WIRE_V1_MAX_NODES
                || (size_t)count > r->size - r->pos) return 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (!check_node(r, depth + 1)) return 0;
        }
        return 1;
    }
    return 0;
}
static int valid_payload(const uint8_t* payload, size_t n) {
    if (!payload || n < 1 || n > ORES_ACTOR_WIRE_V1_MAX_FRAME
            - ORES_ACTOR_WIRE_V1_HEADER_SIZE) return 0;
    WireReader reader = {payload, n, 0, 0};
    return check_node(&reader, 0) && reader.pos == n;
}
int ores_actor_wire_v1_pack(uint64_t schema, uint32_t variant,
                            const uint8_t* payload, size_t size,
                            OresActorWireV1Frame* out) {
    if (!out || out->bytes || out->size || !schema || !variant
            || !valid_payload(payload, size)) return 0;
    size_t total = ORES_ACTOR_WIRE_V1_HEADER_SIZE + size;
    uint8_t* owned = (uint8_t*)malloc(total);
    if (!owned) return 0;
    owned[0] = 'O'; owned[1] = 'M'; owned[2] = 'S'; owned[3] = 'G';
    owned[4] = 1; owned[5] = 0; owned[6] = 0; owned[7] = 0;
    put64(owned + 8, schema);
    put32(owned + 16, variant);
    put32(owned + 20, (uint32_t)size);
    memcpy(owned + ORES_ACTOR_WIRE_V1_HEADER_SIZE, payload, size);
    out->bytes = owned;
    out->size = total;
    return 1;
}
int ores_actor_wire_v1_unpack(const uint8_t* frame, size_t frame_size,
                              uint64_t schema, uint32_t variant,
                              OresActorWireV1Frame* out) {
    if (!out || out->bytes || out->size || !frame || !schema || !variant
            || frame_size < ORES_ACTOR_WIRE_V1_HEADER_SIZE
            || frame_size > ORES_ACTOR_WIRE_V1_MAX_FRAME) return 0;
    if (memcmp(frame, "OMSG", 4) || frame[4] != 1 || frame[5] != 0
            || frame[6] != 0 || frame[7] != 0
            || get64(frame + 8) != schema || get32(frame + 16) != variant
            || get32(frame + 20)
               != frame_size - ORES_ACTOR_WIRE_V1_HEADER_SIZE
            || !valid_payload(frame + ORES_ACTOR_WIRE_V1_HEADER_SIZE,
                              frame_size - ORES_ACTOR_WIRE_V1_HEADER_SIZE)) {
        return 0;
    }
    uint8_t* owned = (uint8_t*)malloc(frame_size);
    if (!owned) return 0;
    memcpy(owned, frame, frame_size);
    out->bytes = owned;
    out->size = frame_size;
    return 1;
}
void ores_actor_wire_v1_release(OresActorWireV1Frame* owned) {
    if (!owned) return;
    if (owned->bytes) {
        volatile uint8_t* p = (volatile uint8_t*)owned->bytes;
        for (size_t i = 0; i < owned->size; ++i) p[i] = 0;
        free(owned->bytes);
    }
    owned->bytes = NULL;
    owned->size = 0;
}
