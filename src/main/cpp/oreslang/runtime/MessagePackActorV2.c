#include "oreslang/runtime/MessagePackActorV2.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Fail closed: any malformed byte or unsupported shape rejects before a
 * queue slot is reserved. No allocation based on untrusted encoded lengths. */
typedef struct {
    const uint8_t* bytes;
    size_t len, at, nodes;
} Reader;
static int take(Reader* r, size_t n, const uint8_t** out) {
    if (n > r->len - r->at) return 0;
    if (out) *out = r->bytes + r->at;
    r->at += n;
    return 1;
}
static int octet(Reader* r, uint8_t* out) {
    const uint8_t* p;
    if (!take(r, 1, &p)) return 0;
    *out = *p;
    return 1;
}
static int uint_be(Reader* r, unsigned n, uint64_t* out) {
    const uint8_t* p;
    if (n > 8 || !take(r, n, &p)) return 0;
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v = (v << 8) | p[i];
    *out = v;
    return 1;
}
static int utf8(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t a = p[i++];
        if (a < 128) continue;
        unsigned extra = 0;
        uint8_t lo = 0x80, hi = 0xbf;
        if (a >= 0xc2 && a <= 0xdf) extra = 1;
        else if (a == 0xe0) { extra = 2; lo = 0xa0; }
        else if (a >= 0xe1 && a <= 0xec) extra = 2;
        else if (a == 0xed) { extra = 2; hi = 0x9f; }
        else if (a >= 0xee && a <= 0xef) extra = 2;
        else if (a == 0xf0) { extra = 3; lo = 0x90; }
        else if (a >= 0xf1 && a <= 0xf3) extra = 3;
        else if (a == 0xf4) { extra = 3; hi = 0x8f; }
        else return 0;
        if (n - i < extra || p[i] < lo || p[i] > hi) return 0;
        ++i;
        for (unsigned j = 1; j < extra; ++j, ++i)
            if (p[i] < 0x80 || p[i] > 0xbf) return 0;
    }
    return 1;
}
static int string(Reader* r, const uint8_t** data, size_t* len) {
    uint8_t t;
    uint64_t n = 0;
    if (!octet(r, &t)) return 0;
    if ((t & 0xe0) == 0xa0) n = t & 31;
    else if (t == 0xd9) {
        if (!uint_be(r, 1, &n) || n <= 31) return 0;
    } else if (t == 0xda) {
        if (!uint_be(r, 2, &n) || n <= 255) return 0;
    } else if (t == 0xdb) {
        if (!uint_be(r, 4, &n) || n <= 65535) return 0;
    } else return 0;
    if (n > ORES_MP2_MAX_BYTES || !take(r, (size_t)n, data)
            || !utf8(*data, (size_t)n)) return 0;
    *len = (size_t)n;
    return 1;
}
static int array_header(Reader* r, size_t* out) {
    uint8_t t;
    uint64_t n = 0;
    if (!octet(r, &t)) return 0;
    if ((t & 0xf0) == 0x90) n = t & 15;
    else if (t == 0xdc) {
        if (!uint_be(r, 2, &n) || n <= 15) return 0;
    } else if (t == 0xdd) {
        if (!uint_be(r, 4, &n) || n <= 65535) return 0;
    } else return 0;
    if (n > ORES_MP2_MAX_NODES) return 0;
    *out = (size_t)n;
    return 1;
}
static int signed_integer(Reader* r) {
    uint8_t t;
    uint64_t bits = 0;
    if (!octet(r, &t)) return 0;
    if (t <= 0x7f || t >= 0xe0) return 1;
    unsigned width;
    switch (t) {
        case 0xcc: case 0xd0: width = 1; break;
        case 0xcd: case 0xd1: width = 2; break;
        case 0xce: case 0xd2: width = 4; break;
        case 0xcf: case 0xd3: width = 8; break;
        default: return 0;
    }
    if (!uint_be(r, width, &bits)) return 0;
    if (t == 0xcc) return bits > 127;
    if (t == 0xcd) return bits > 255;
    if (t == 0xce) return bits > 65535;
    if (t == 0xcf) return bits > UINT64_C(0xffffffff) && bits <= INT64_MAX;
    /* Signed negative classes; positive values must use unsigned forms. */
    int64_t number;
    if (width == 1) number = (int8_t)bits;
    else if (width == 2) number = (int16_t)bits;
    else if (width == 4) number = (int32_t)bits;
    else number = (int64_t)bits;
    if (t == 0xd0) return number < -32;
    if (t == 0xd1) return number < INT8_MIN;
    if (t == 0xd2) return number < INT16_MIN;
    return number < INT32_MIN;
}
/* Only compiler-generated canonical descriptors are admissible. Parser is
 * bounded separately from the MessagePack value to avoid recursion attacks. */
static int valid_shape(const char** s, unsigned depth) {
    if (depth > ORES_MP2_MAX_DEPTH) return 0;
    char t = **s;
    if (t == 'B' || t == 'I' || t == 'S' ||
        (t == 'F' && !(!strncmp(*s, "FA[", 3) || !strncmp(*s, "FL[", 3)))) {
        ++*s; return 1;
    }
    if (!strncmp(*s, "FA[", 3) || !strncmp(*s, "FL[", 3))
        *s += 3;
    else if (t == 'T' && (*s)[1] == '[') *s += 2;
    else if ((t == 'A' || t == 'L') && (*s)[1] == '[') {
        *s += 2;
        if (!valid_shape(s, depth + 1) || **s != ']') return 0;
        ++*s; return 1;
    } else return 0;
    if (**s == ']') { ++*s; return 1; }
    for (;;) {
        if (!valid_shape(s, depth + 1)) return 0;
        if (**s == ']') { ++*s; return 1; }
        if (**s != ',') return 0;
        ++*s;
    }
}
static int typed_value(Reader* r, const char** shape, unsigned depth) {
    if (depth > ORES_MP2_MAX_DEPTH || ++r->nodes > ORES_MP2_MAX_NODES)
        return 0;
    char t = **shape;
    if (t == 'B') {
        uint8_t b; ++*shape;
        return octet(r, &b) && (b == 0xc2 || b == 0xc3);
    }
    if (t == 'I') { ++*shape; return signed_integer(r); }
    if (t == 'F' && strncmp(*shape, "FA[", 3) && strncmp(*shape, "FL[", 3)) {
        uint8_t b; uint64_t bits;
        ++*shape;
        if (!octet(r, &b) || b != 0xcb || !uint_be(r, 8, &bits)) return 0;
        double number;
        memcpy(&number, &bits, 8);
        return isfinite(number);
    }
    if (t == 'S') {
        const uint8_t* data; size_t n;
        ++*shape; return string(r, &data, &n);
    }
    int fixed = 0;
    if (!strncmp(*shape, "FA[", 3) || !strncmp(*shape, "FL[", 3)) {
        *shape += 3; fixed = 1;
    } else if (t == 'T' && (*shape)[1] == '[') {
        *shape += 2; fixed = 1;
    } else if ((t == 'A' || t == 'L') && (*shape)[1] == '[') {
        *shape += 2;
    } else return 0;
    size_t count;
    if (!array_header(r, &count)
            || count > r->len - r->at
            || count > ORES_MP2_MAX_NODES - r->nodes) return 0;
    if (fixed) {
        for (size_t i = 0; i < count; ++i) {
            if (i > 0 && *(*shape)++ != ',') return 0;
            if (!typed_value(r, shape, depth + 1)) return 0;
        }
        if (**shape != ']') return 0;
        ++*shape;
        return 1;
    }
    const char* one_shape = *shape;
    const char* end_shape = one_shape;
    if (!valid_shape(&end_shape, depth + 1) || *end_shape != ']') return 0;
    for (size_t i = 0; i < count; ++i) {
        const char* s = one_shape;
        if (!typed_value(r, &s, depth + 1) || s != end_shape) return 0;
    }
    *shape = end_shape + 1;
    return 1;
}
int ores_mp2_validate_copy(const OresMp2Contract* c,
                           const uint8_t* bytes, size_t size,
                           OresMp2Owned* out) {
    if (!c || !c->schema_id || !c->shape || !c->shape_sha256 || !bytes
            || !out || out->bytes || out->size
            || size > ORES_MP2_MAX_BYTES || size < 40) return 0;
    size_t schema_len = strlen(c->schema_id);
    if (!schema_len || schema_len > 256 || strlen(c->shape) > 4096) return 0;
    for (size_t i = 0; i < schema_len; ++i)
        if ((unsigned char)c->schema_id[i] < 33
                || (unsigned char)c->schema_id[i] > 126) return 0;
    const char* shape_end = c->shape;
    if (!valid_shape(&shape_end, 0) || *shape_end) return 0;
    Reader r = {bytes, size, 0, 0};
    size_t n;
    uint8_t version;
    const uint8_t *schema, *digest;
    size_t actual_schema_len;
    if (!array_header(&r, &n) || n != 4
            || !octet(&r, &version) || version != 2
            || !string(&r, &schema, &actual_schema_len)
            || actual_schema_len != schema_len
            || memcmp(schema, c->schema_id, schema_len)
            || !octet(&r, &version) || version != 0xc4
            || !octet(&r, &version) || version != 32
            || !take(&r, 32, &digest)
            || memcmp(digest, c->shape_sha256, 32)) return 0;
    const char* shape = c->shape;
    if (!typed_value(&r, &shape, 0) || *shape || r.at != size) return 0;
    uint8_t* copy = malloc(size);
    if (!copy) return 0;
    memcpy(copy, bytes, size);
    out->bytes = copy; out->size = size;
    return 1;
}
void ores_mp2_owned_release(OresMp2Owned* v) {
    if (!v) return;
    if (v->bytes) {
        volatile uint8_t* p = (volatile uint8_t*)v->bytes;
        for (size_t i = 0; i < v->size; ++i) p[i] = 0;
        free(v->bytes);
    }
    v->bytes = NULL; v->size = 0;
}

struct OresMp2ActorGroup {
    OresNativeActorScheduler* scheduler;
    OresMp2Contract* contracts;
    unsigned char* spawned;
    size_t actors;
    OresMp2OnMessage callback;
    void* callback_context;
};
/* Scheduler's DATA callback holds the only reference to the admitted owned
 * packet. After user callback returns, its memory is zeroized and released.
 * CONTROL continues to have an independent reserved slot in the scheduler. */
static void deliver(void* raw, size_t slot, uint64_t token) {
    OresMp2ActorGroup* group = raw;
    OresMp2Owned* owned = (OresMp2Owned*)(uintptr_t)token;
    if (!owned || !owned->bytes || slot >= group->actors) abort();
    group->callback(group->callback_context, slot, owned->bytes, owned->size);
    ores_mp2_owned_release(owned);
    free(owned);
}
static OresMp2ActorRef wrap(OresNativeActorRef r) {
    OresMp2ActorRef result = {r.scheduler_domain, r.actor_slot, r.generation};
    return result;
}
static OresNativeActorRef unwrap(OresMp2ActorRef r) {
    OresNativeActorRef result = {r.scheduler_domain, r.actor_slot, r.generation};
    return result;
}
OresMp2ActorGroup* ores_mp2_group_open(size_t actors, size_t capacity,
                                      size_t carriers, OresMp2OnMessage callback,
                                      void* context) {
    if (!callback || sizeof(uintptr_t) != sizeof(uint64_t) || !actors) return NULL;
    OresMp2ActorGroup* g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->contracts = calloc(actors, sizeof(*g->contracts));
    g->spawned = calloc(actors, 1);
    if (!g->contracts || !g->spawned) { free(g->spawned); free(g->contracts); free(g); return NULL; }
    g->actors = actors; g->callback = callback; g->callback_context = context;
    g->scheduler = ores_native_actor_scheduler_open(actors, capacity, carriers,
                                                    deliver, g);
    if (!g->scheduler) { free(g->spawned); free(g->contracts); free(g); return NULL; }
    return g;
}
int ores_mp2_group_contract(OresMp2ActorGroup* g, size_t slot,
                            OresMp2Contract contract) {
    if (!g || slot >= g->actors || g->contracts[slot].schema_id
            || !contract.schema_id || !contract.shape || !contract.shape_sha256
            || g->spawned[slot]) return 0;
    /* Configuration is single-threaded and must finish before guest send. */
    g->contracts[slot] = contract;
    return 1;
}
OresMp2ActorRef ores_mp2_group_spawn(OresMp2ActorGroup* g, size_t slot) {
    OresMp2ActorRef empty = {0, 0, 0};
    if (!g || slot >= g->actors || !g->contracts[slot].schema_id || g->spawned[slot]) return empty;
    OresMp2ActorRef ref = wrap(ores_native_actor_scheduler_spawn(g->scheduler, slot));
    if (ref.generation) g->spawned[slot] = 1;
    return ref;
}
int ores_mp2_group_send(OresMp2ActorGroup* g, OresMp2ActorRef target,
                        const uint8_t* wire, size_t size) {
    if (!g || target.actor_slot >= g->actors
            || !g->contracts[target.actor_slot].schema_id) return 0;
    OresMp2Owned* snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) return 0;
    if (!ores_mp2_validate_copy(&g->contracts[target.actor_slot],
                                wire, size, snapshot)) {
        free(snapshot); return 0;
    }
    if (!ores_native_actor_scheduler_send(g->scheduler, unwrap(target),
                                          (uint64_t)(uintptr_t)snapshot)) {
        ores_mp2_owned_release(snapshot); free(snapshot);
        return 0;
    }
    return 1;
}
int ores_mp2_group_stop(OresMp2ActorGroup* g, OresMp2ActorRef target) {
    return g && ores_native_actor_scheduler_stop(g->scheduler, unwrap(target));
}
int ores_mp2_group_wait_done(OresMp2ActorGroup* g, OresMp2ActorRef target) {
    return g && ores_native_actor_scheduler_wait_done(g->scheduler, unwrap(target));
}
int ores_mp2_group_destroy(OresMp2ActorGroup* g) {
    if (!g) return 0;
    ores_native_actor_scheduler_retire(g->scheduler);
    if (!ores_native_actor_scheduler_drain(g->scheduler)
            || !ores_native_actor_scheduler_try_destroy(g->scheduler)) return 0;
    free(g->spawned);
    free(g->contracts);
    free(g);
    return 1;
}
