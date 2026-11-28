#include "atto/host_fs_signed_ceiling.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

/* TEST ONLY: generate a fresh key, never ship any production signing secret. */
static EVP_PKEY *ephemeral_signer(uint8_t public_key[32]) {
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    assert(context != NULL && EVP_PKEY_keygen_init(context) == 1);
    EVP_PKEY *key = NULL;
    assert(EVP_PKEY_keygen(context, &key) == 1);
    size_t size = 32;
    assert(EVP_PKEY_get_raw_public_key(key, public_key, &size) == 1 && size == 32);
    EVP_PKEY_CTX_free(context);
    return key;
}
static size_t make_descriptor(uint8_t *wire, const char *artifact,
                              const char *read_scope, const char *write_scope,
                              const char *deny_read, const char *deny_write) {
    const char *fields[] = {read_scope, write_scope, deny_read, deny_write};
    const size_t magic_size = sizeof(ATTO_FS_CEILING_MAGIC) - 1u;
    memcpy(wire, ATTO_FS_CEILING_MAGIC, magic_size);
    unsigned int hash_len = 0;
    assert(EVP_Digest(artifact, strlen(artifact), wire + magic_size,
                      &hash_len, EVP_sha256(), NULL) == 1 && hash_len == 32);
    size_t pos = magic_size + 32u;
    wire[pos++] = 0xD1u;
    for (unsigned i = 0; i < 4; ++i) {
        const size_t n = strlen(fields[i]); assert(n < 1024u);
        wire[pos++] = (uint8_t)(n & 0xffu);
        wire[pos++] = (uint8_t)(n >> 8u);
        memcpy(wire + pos, fields[i], n); pos += n;
    }
    return pos;
}
static void sign_descriptor(EVP_PKEY *signer, const uint8_t *wire, size_t size, uint8_t signature[64]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new(); assert(ctx != NULL);
    assert(EVP_DigestSignInit(ctx, NULL, NULL, NULL, signer) == 1);
    size_t signature_len = 64u;
    assert(EVP_DigestSign(ctx, signature, &signature_len, wire, size) == 1 && signature_len == 64u);
    EVP_MD_CTX_free(ctx);
}
static void denied(const char *artifact, const uint8_t *wire, size_t n, const uint8_t sig[64],
                   const uint8_t key[32], atto_fs_verified_ceiling *result) {
    memset(result, 0xAA, sizeof(*result));
    assert(atto_fs_verify_signed_ceiling_v1((const uint8_t*)artifact, strlen(artifact),
                                            wire, n, sig, key, result) != 0);
    const uint8_t *raw = (const uint8_t *)result;
    for (size_t i = 0; i < sizeof(*result); ++i) assert(raw[i] == 0);
}
int main(void) {
    const char *artifact = "compiled-atto-guest-v5-example";
    uint8_t key[32], other_key[32], wire[5000]={0}, sig[64];
    EVP_PKEY *signer = ephemeral_signer(key);
    EVP_PKEY *other = ephemeral_signer(other_key);
    atto_fs_verified_ceiling scope;
    size_t n = make_descriptor(wire, artifact, "data", "out", "data/secret", "out/locked");
    sign_descriptor(signer, wire, n, sig);
    assert(atto_fs_verify_signed_ceiling_v1((const uint8_t*)artifact, strlen(artifact),wire,n,sig,key,&scope)==0);
    assert(strcmp(scope.allow_read,"data")==0 && strcmp(scope.allow_write,"out")==0);
    assert(strcmp(scope.deny_read,"data/secret")==0 && strcmp(scope.deny_write,"out/locked")==0);
    denied("compiled-atto-guest-v5-example-tampered",wire,n,sig,key,&scope);
    denied(artifact,wire,n,sig,other_key,&scope);
    sig[12] ^= 1u;
    denied(artifact,wire,n,sig,key,&scope);
    sig[12] ^= 1u;
    denied(artifact,wire,n-1u,sig,key,&scope);
    wire[n++] = 0u;
    sign_descriptor(signer,wire,n,sig);
    denied(artifact,wire,n,sig,key,&scope); // signed trailing bytes

    n = make_descriptor(wire,artifact,"../etc","out","","");
    sign_descriptor(signer,wire,n,sig);
    denied(artifact,wire,n,sig,key,&scope);
    n = make_descriptor(wire,artifact,"data//foo","out","","");
    sign_descriptor(signer,wire,n,sig);
    denied(artifact,wire,n,sig,key,&scope);
    n = make_descriptor(wire,artifact,"data","out","","");
    const size_t prefix = sizeof(ATTO_FS_CEILING_MAGIC) - 1u;
    wire[prefix + 32u] = 0u; // missing explicit deny marker, signed anyway
    sign_descriptor(signer,wire,n,sig);
    denied(artifact,wire,n,sig,key,&scope);
    n = make_descriptor(wire,artifact,"data","out","","");
    wire[prefix+33u+2u] = 0u; // embedded NUL in signed scope
    sign_descriptor(signer,wire,n,sig);
    denied(artifact,wire,n,sig,key,&scope);
    n = make_descriptor(wire,artifact,"data","out","","");
    wire[prefix+33u+2u] = 0xFFu; // non-ASCII signed scope
    sign_descriptor(signer,wire,n,sig);
    denied(artifact,wire,n,sig,key,&scope);
    n = make_descriptor(wire,artifact,"data","out","","");
    wire[prefix+33u] = 0xFFu; wire[prefix+33u+1u] = 0x7Fu; // overlarge length
    sign_descriptor(signer,wire,n,sig);
    denied(artifact,wire,n,sig,key,&scope);
    n = make_descriptor(wire,artifact,"","","","");
    sign_descriptor(signer,wire,n,sig);
    assert(atto_fs_verify_signed_ceiling_v1((const uint8_t*)artifact,strlen(artifact),wire,n,sig,key,&scope)==0);
    assert(scope.allow_read[0]==0 && scope.allow_write[0]==0);
    n = make_descriptor(wire,artifact,"*","out","out/private","");
    sign_descriptor(signer,wire,n,sig);
    assert(atto_fs_verify_signed_ceiling_v1((const uint8_t*)artifact,strlen(artifact),wire,n,sig,key,&scope)==0);
    assert(strcmp(scope.allow_read,"*")==0);
    assert(atto_fs_verify_signed_ceiling_v1(NULL,0,wire,n,sig,key,&scope)!=0);
    assert(atto_fs_verify_signed_ceiling_v1((const uint8_t*)artifact,strlen(artifact),wire,n,sig,key,NULL)==-EINVAL);
    EVP_PKEY_free(signer); EVP_PKEY_free(other);
    puts("ATTO_HOST_FS_SIGNED_CEILING_TEST_PASS");
    return 0;
}
