#include "oreslang/runtime/MessagePackActorV2.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* This independently exercises production native admission, not just the
 * compiler's encoder. The golden is shared with Java/GraalVM exactly.
 */
static const char golden[] =
    "9402d98f6f7265736c616e672e656e756d2e306162326236366364343839"
    "373130373332323563653264353938323735336137343830326636653261316437393861"
    "306632646364386665383233643232612e326434356430383134646535323635373039"
    "326261626636363331323437633761623136303164643035373935646432616534323833"
    "65303665383036373039c42071605bdfa3efdbcb5fe7aa4d4230f049269d4cd630b"
    "72c8c27f452c9b09ea42093a3466f6f0192a342617201";
static unsigned from_hex(char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    assert(c >= 'a' && c <= 'f');
    return (unsigned)(c - 'a' + 10);
}
int main(void) {
    uint8_t bytes[193];
    assert(strlen(golden) == 386);
    for (size_t i = 0; i < 193; ++i)
        bytes[i] = (uint8_t)((from_hex(golden[2*i]) << 4) |
                              from_hex(golden[2*i+1]));
    assert(bytes[0] == 0x94 && bytes[1] == 2 &&
           bytes[2] == 0xd9 && bytes[3] == 143);
    char schema[144];
    memcpy(schema, bytes + 4, 143);
    schema[143] = 0;
    const char* shape = "T[S,I,E(3:Bar)[1:8:Message1=N;]]";
    OresMp2Contract c = {schema, shape, bytes + 4 + 143 + 2};
    OresMp2Owned frame = {bytes, sizeof(bytes)};
    assert(ores_mp2_contract_valid(&c));
    assert(ores_mp2_validate_owned(&c, &frame));
    assert(ores_mp2_tagged_identity_matches_validated(&c, &frame, "Foo", 1));

    /* The inner tag is bound to a compiler-owned case; a different int
     * must not qualify merely because the outer schema digest is unchanged. */
    bytes[192] = 2;
    assert(!ores_mp2_validate_owned(&c, &frame));
    bytes[192] = 1;
    /* Same length/valid UTF-8, but different nominal owner. */
    bytes[189] = 'F';
    assert(!ores_mp2_validate_owned(&c, &frame));
    bytes[189] = 'B';

    /* The full descriptor, not wire hash bytes alone, is checked. */
    OresMp2Contract malformed = {schema, "T[S,I,E(3:Bar)[1:8:Message1=N]]",
                                 bytes + 4 + 143 + 2};
    assert(!ores_mp2_contract_valid(&malformed));
    OresMp2Contract wrong = {schema, "T[S,I,E(3:Baz)[1:8:Message1=N;]]",
                             bytes + 4 + 143 + 2};
    assert(!ores_mp2_contract_valid(&wrong));
    /* Host-supplied descriptors are not necessarily compiler-generated.
     * A malicious length must never index past its NUL terminator, even
     * when the original digest would already reject the proposed shape.
     * This exercises the actual validator under ASan/UBSan. */
    const char* truncated[] = {
        "E(", "E(256:", "E(3:", "E(3:Ba",
        "E(3:Bar)", "E(3:Bar)[",
        "E(3:Bar)[1:", "E(3:Bar)[1:256:",
        "E(3:Bar)[1:8:", "E(3:Bar)[1:8:Message1=",
        "E(3:Bar)[1:8:Message1=N", "E(3:Bar)[1:8:Message1=N;",
        "E(3:Bar)[01:8:Message1=N;]",
        "E(3:Bar)[1:08:Message1=N;]",
        "E(3:Bar)[0:8:Message1=N;]",
        "E(3:Bar)[4294967296:8:Message1=N;]",
        "E(3:Bar)[1:8:Message1=N;1:2:No=N;]",
        "E(3:Bar)[1:8:Message1=N;0:2:No=N;]",
        "E(3:Bar)[1:8:Message1=N;2:5:Wrong=N"
    };
    for (size_t i = 0; i < sizeof(truncated)/sizeof(truncated[0]); ++i) {
        OresMp2Contract broken = {schema, truncated[i], c.shape_sha256};
        assert(!ores_mp2_contract_valid(&broken));
    }
    /* Mutate every byte of the authenticated frame: any single-byte
     * change to this minimal nested-nullary golden must be rejected. */
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        const uint8_t old = bytes[i];
        bytes[i] ^= 0x01;
        /* Structural T[S,I,E(...)] validation alone cannot bind the
         * outer enum owner or tag. Production source_send_binary
         * ALWAYS applies the separate nominal identity gate afterwards. */
        assert(!ores_mp2_validate_owned(&c, &frame)
               || !ores_mp2_tagged_identity_matches_validated(
                   &c, &frame, "Foo", 1));
        bytes[i] = old;
    }
    assert(ores_mp2_validate_owned(&c, &frame));
    printf("NESTED_ENUM_NATIVE_RUNTIME_PASS bytes=%zu negatives=%zu\n",
           sizeof(bytes), sizeof(truncated)/sizeof(truncated[0]) + sizeof(bytes));
    return 0;
}
