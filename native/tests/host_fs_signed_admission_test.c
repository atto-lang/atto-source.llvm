#define _GNU_SOURCE
#include "atto/host_fs_signed_admission.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static EVP_PKEY *ephemeral_key(unsigned char pubkey[32]) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    assert(ctx && EVP_PKEY_keygen_init(ctx) == 1);
    EVP_PKEY *key = NULL;
    assert(EVP_PKEY_keygen(ctx, &key) == 1);
    size_t pub_size = 32u;
    assert(EVP_PKEY_get_raw_public_key(key, pubkey, &pub_size) == 1 && pub_size == 32u);
    EVP_PKEY_CTX_free(ctx);
    return key;
}
static size_t signed_wire(unsigned char *out, const unsigned char *artifact,
                          size_t artifact_len, const char *const fields[4]) {
    const size_t prefix = sizeof(ATTO_FS_CEILING_MAGIC) - 1u;
    memcpy(out, ATTO_FS_CEILING_MAGIC, prefix);
    unsigned int hash_len = 0u;
    assert(EVP_Digest(artifact, artifact_len, out + prefix, &hash_len,
                      EVP_sha256(), NULL) == 1 && hash_len == 32u);
    size_t pos = prefix + 32u;
    out[pos++] = 0xD1u; /* explicit denies present */
    for (unsigned i = 0; i < 4u; ++i) {
        const size_t n = strlen(fields[i]);
        assert(n < ATTO_FS_SIGNED_SCOPE_LIMIT);
        out[pos++] = (unsigned char)(n & 255u);
        out[pos++] = (unsigned char)(n >> 8u);
        memcpy(out + pos, fields[i], n);
        pos += n;
    }
    return pos;
}
static void sign(EVP_PKEY *key, const unsigned char *wire, size_t length,
                 unsigned char signature[64]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    assert(ctx);
    assert(EVP_DigestSignInit(ctx, NULL, NULL, NULL, key) == 1);
    size_t written = 64u;
    assert(EVP_DigestSign(ctx, signature, &written, wire, length) == 1 && written == 64u);
    EVP_MD_CTX_free(ctx);
}
static void join(char *dst, size_t capacity, const char *base, const char *rel) {
    const int n = snprintf(dst, capacity, "%s/%s", base, rel);
    assert(n > 0 && (size_t)n < capacity);
}
static void put_file(const char *path, const char *bytes) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const size_t n = strlen(bytes);
    assert(write(fd, bytes, n) == (ssize_t)n);
    assert(close(fd) == 0);
}
static void expect_file(const char *path, const char *expected) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    char result[80] = {0};
    const ssize_t n = read(fd, result, sizeof(result));
    assert(n == (ssize_t)strlen(expected));
    assert(memcmp(result, expected, (size_t)n) == 0);
    assert(close(fd) == 0);
}
int main(void) {
    char tmp[] = "/tmp/atto-fs-signed-admission-XXXXXX";
    assert(mkdtemp(tmp) != NULL);
    char root[512], data[512], out[512], src[512], dst[512], secret[512];
    join(root, sizeof(root), tmp, "root"); assert(mkdir(root, 0700) == 0);
    join(data, sizeof(data), root, "data"); assert(mkdir(data, 0700) == 0);
    join(out, sizeof(out), root, "out"); assert(mkdir(out, 0700) == 0);
    char private_dir[512];
    join(private_dir, sizeof(private_dir), data, "private");
    assert(mkdir(private_dir, 0700) == 0);
    join(src, sizeof(src), data, "input.txt"); put_file(src, "signed-data");
    join(secret, sizeof(secret), private_dir, "secret.txt"); put_file(secret, "secret");
    join(dst, sizeof(dst), out, "copy.txt");

    const unsigned char artifact[] = "verified-compiled-guest-image-bytes";
    unsigned char pubkey[32], otherkey[32], signature[64], wire[5000] = {0};
    EVP_PKEY *signer = ephemeral_key(pubkey);
    EVP_PKEY *other = ephemeral_key(otherkey);
    const char *const fields[4] = {"data", "out", "data/private", "out/private"};
    const size_t wire_size = signed_wire(wire, artifact, sizeof(artifact) - 1u, fields);
    sign(signer, wire, wire_size, signature);

    atto_fs_signed_launch launch = {
        .artifact_bytes = artifact, .artifact_size = sizeof(artifact) - 1u,
        .signed_descriptor = wire, .signed_descriptor_size = wire_size,
        .signature_ed25519 = signature, .trusted_public_key = pubkey,
        .trusted_root = root, .tenant_id = 99u, .generation = 4u,
        .host = {"*", "*", NULL, NULL},
        .runtime = {"data", "out", NULL, NULL},
        .tenant = {"*", "*", NULL, NULL},
        .actor = {"data", "out", NULL, NULL},
        .delegation = {"*", "*", NULL, NULL}
    };
    atto_fs_broker broker = {0};
    assert(atto_fs_broker_admit_signed(&broker, &launch) == 0);
    assert(broker.policy_layer_count == ATTO_FS_POLICY_LAYER_COUNT);
    unsigned char bytes[64] = {0};
    size_t used = 0u;
    assert(atto_fs_broker_read(&broker, 99u, 4u, "data/input.txt",
                               bytes, sizeof(bytes), &used) == 0);
    assert(used == strlen("signed-data") && memcmp(bytes, "signed-data", used) == 0);
    assert(atto_fs_broker_write(&broker, 99u, 4u, "out/copy.txt", bytes, used) == 0);
    expect_file(dst, "signed-data");
    assert(atto_fs_broker_read(&broker, 99u, 4u, "data/private/secret.txt",
                               bytes, sizeof(bytes), &used) == -EACCES);
    assert(atto_fs_broker_write(&broker, 99u, 4u, "data/input.txt",
                                (const unsigned char *)"bad", 3u) == -EACCES);
    expect_file(src, "signed-data");
    assert(atto_fs_broker_revoke(&broker, 99u, 4u) == 0);
    assert(atto_fs_broker_write(&broker, 99u, 4u, "out/copy.txt",
                                (const unsigned char *)"bad", 3u) == -EACCES);
    atto_fs_broker_close(&broker);

    /* Every signed-verification failure MUST precede broker creation. */
    launch.trusted_public_key = otherkey;
    assert(atto_fs_broker_admit_signed(&broker, &launch) == -EACCES);
    assert(!broker.initialized);
    launch.trusted_public_key = pubkey;

    unsigned char bad_artifact[] = "verified-compiled-guest-image-bytes";
    bad_artifact[2] ^= 1u;
    launch.artifact_bytes = bad_artifact;
    assert(atto_fs_broker_admit_signed(&broker, &launch) == -EACCES);
    assert(!broker.initialized);
    launch.artifact_bytes = artifact;

    signature[8] ^= 1u;
    assert(atto_fs_broker_admit_signed(&broker, &launch) == -EACCES);
    assert(!broker.initialized);
    signature[8] ^= 1u;

    launch.signed_descriptor_size = wire_size - 1u;
    assert(atto_fs_broker_admit_signed(&broker, &launch) == -EACCES);
    launch.signed_descriptor_size = wire_size;

    /* No unsiged broadening: the signed artifact grants out but runtime
     * denies it. A trusted launch policy can ONLY reduce permissions. */
    launch.runtime.allow_write = NULL;
    assert(atto_fs_broker_admit_signed(&broker, &launch) == 0);
    assert(atto_fs_broker_write(&broker, 99u, 4u, "out/copy.txt",
                                (const unsigned char *)"bad", 3u) == -EACCES);
    atto_fs_broker_close(&broker);
    launch.runtime.allow_write = "out";

    launch.actor.deny_write = "out/copy.txt";
    assert(atto_fs_broker_admit_signed(&broker, &launch) == 0);
    assert(atto_fs_broker_write(&broker, 99u, 4u, "out/copy.txt",
                                (const unsigned char *)"bad", 3u) == -EACCES);
    atto_fs_broker_close(&broker);
    launch.actor.deny_write = NULL;
    expect_file(dst, "signed-data");

    /* Even a correctly signed but malformed artifact maximum is rejected. */
    const char *const malformed[4] = {"../outside", "out", "", ""};
    const size_t malformed_size = signed_wire(wire, artifact, sizeof(artifact)-1u, malformed);
    sign(signer, wire, malformed_size, signature);
    launch.signed_descriptor_size = malformed_size;
    assert(atto_fs_broker_admit_signed(&broker, &launch) == -EACCES);
    assert(!broker.initialized);

    EVP_PKEY_free(other);
    EVP_PKEY_free(signer);
    puts("ATTO_FS_SIGNED_ADMISSION_REAL_BROKER_HOST_ONLY_PASS");
    return 0;
}
