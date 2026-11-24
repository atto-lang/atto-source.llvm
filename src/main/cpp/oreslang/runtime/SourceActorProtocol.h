#ifndef ORESLANG_SOURCE_ACTOR_PROTOCOL_H
#define ORESLANG_SOURCE_ACTOR_PROTOCOL_H

/* Private table and wire codec for the bounded transition-only actor profile.
 * The bootstrap owns every instance and serializes access under group.mutex. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    PROTOCOL_COMMITTED = 1,
    PROTOCOL_ILLEGAL_EDGE,
    PROTOCOL_INVALID_PAYLOAD,
    PROTOCOL_INVALID_ENVELOPE,
    PROTOCOL_INTERRUPTED,
};

enum {
    PROTOCOL_MAX_STATES = 128,
    PROTOCOL_MAX_MESSAGES = 256,
    PROTOCOL_MAX_EDGES = 256,
    PROTOCOL_MAX_FIELDS = 16,
    PROTOCOL_DIGEST_SIZE = 32,
    PROTOCOL_HEADER_SIZE = 39,
    PROTOCOL_MAX_FRAME = PROTOCOL_HEADER_SIZE + 8 * PROTOCOL_MAX_FIELDS,
};

typedef struct {
    uint16_t count;
    struct { int64_t minimum, maximum; } bounds[PROTOCOL_MAX_FIELDS];
} SourceProtocolMessage;

typedef struct { uint16_t from, tag, to; } SourceProtocolEdge;

typedef struct {
    size_t length;
    int occupied;
    uint8_t bytes[PROTOCOL_MAX_FRAME];
} SourceProtocolFrame;

typedef struct {
    uint8_t digest[PROTOCOL_DIGEST_SIZE];
    uint16_t state;
    uint16_t message_count, edge_count;
    SourceProtocolMessage messages[PROTOCOL_MAX_MESSAGES];
    SourceProtocolEdge edges[PROTOCOL_MAX_EDGES];
    SourceProtocolFrame pending[SOURCE_GROUP_MAILBOX];
    size_t pending_count;
    uint8_t receipts[SOURCE_GROUP_MAILBOX][ORES_SOURCE_PROTOCOL_RECEIPT_SIZE];
    size_t receipt_head, receipt_count;
    uint64_t turn;
} SourceProtocol;

static uint16_t protocol_read_u16(const uint8_t* bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int64_t protocol_read_i64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value <= INT64_MAX ? (int64_t)value : -1 - (int64_t)~value;
}

static void protocol_write_u16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void protocol_write_u64(uint8_t* bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static SourceProtocol* protocol_open(const uint8_t* plan, size_t length) {
    /* Digest, initial state, counts, per-message bounds, sorted edge triples. */
    const size_t header_size = PROTOCOL_DIGEST_SIZE + 8;
    if (!plan || length < header_size) return NULL;
    const uint16_t initial = protocol_read_u16(plan + 32);
    const uint16_t states = protocol_read_u16(plan + 34);
    const uint16_t messages = protocol_read_u16(plan + 36);
    const uint16_t edges = protocol_read_u16(plan + 38);
    if (initial >= states || states > PROTOCOL_MAX_STATES ||
        messages > PROTOCOL_MAX_MESSAGES || edges > PROTOCOL_MAX_EDGES)
        return NULL;

    SourceProtocol* protocol = calloc(1, sizeof(*protocol));
    if (!protocol) return NULL;
    memcpy(protocol->digest, plan, sizeof(protocol->digest));
    protocol->state = initial;
    protocol->message_count = messages;
    protocol->edge_count = edges;

    size_t offset = header_size;
    for (size_t i = 0; i < messages; ++i) {
        if (length - offset < 2) goto invalid;
        SourceProtocolMessage* message = &protocol->messages[i];
        message->count = protocol_read_u16(plan + offset);
        offset += 2;
        if (message->count > PROTOCOL_MAX_FIELDS ||
            length - offset < 16u * message->count) goto invalid;
        for (size_t field = 0; field < message->count; ++field) {
            const int64_t minimum = protocol_read_i64(plan + offset);
            const int64_t maximum = protocol_read_i64(plan + offset + 8);
            if (minimum > maximum) goto invalid;
            message->bounds[field].minimum = minimum;
            message->bounds[field].maximum = maximum;
            offset += 16;
        }
    }

    if (length - offset != 6u * edges) goto invalid;
    uint32_t previous = 0;
    for (size_t i = 0; i < edges; ++i) {
        SourceProtocolEdge edge = {
            protocol_read_u16(plan + offset),
            protocol_read_u16(plan + offset + 2),
            protocol_read_u16(plan + offset + 4)
        };
        const uint32_t key = ((uint32_t)edge.from << 16) | edge.tag;
        if (edge.from >= states || edge.to >= states || edge.tag >= messages ||
            (i && key <= previous)) goto invalid;
        protocol->edges[i] = edge;
        previous = key;
        offset += 6;
    }
    return protocol;

invalid:
    free(protocol);
    return NULL;
}

static unsigned protocol_transition(const SourceProtocol* protocol,
        const SourceProtocolFrame* frame, uint16_t* destination) {
    if (frame->length < PROTOCOL_HEADER_SIZE ||
        memcmp(frame->bytes, "ATPM\1", 5) ||
        memcmp(frame->bytes + 5, protocol->digest, sizeof(protocol->digest)))
        return PROTOCOL_INVALID_ENVELOPE;

    const uint16_t tag = protocol_read_u16(frame->bytes + 37);
    if (tag >= protocol->message_count) return PROTOCOL_INVALID_ENVELOPE;
    for (size_t i = 0; i < protocol->edge_count; ++i) {
        const SourceProtocolEdge edge = protocol->edges[i];
        if (edge.from != protocol->state || edge.tag != tag) continue;

        const SourceProtocolMessage* message = &protocol->messages[tag];
        if (frame->length != PROTOCOL_HEADER_SIZE + 8u * message->count)
            return PROTOCOL_INVALID_PAYLOAD;
        for (size_t field = 0; field < message->count; ++field) {
            const int64_t value = protocol_read_i64(
                frame->bytes + PROTOCOL_HEADER_SIZE + 8 * field);
            if (value < message->bounds[field].minimum ||
                value > message->bounds[field].maximum)
                return PROTOCOL_INVALID_PAYLOAD;
        }
        *destination = edge.to;
        return PROTOCOL_COMMITTED;
    }
    return PROTOCOL_ILLEGAL_EDGE;
}

#endif
