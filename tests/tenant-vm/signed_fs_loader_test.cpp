#include "oreslang/runtime/TenantVmSignedFsLoader.hpp"
#include <openssl/evp.h>
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

using namespace oreslang::runtime::vm;
static std::vector<std::uint8_t> read_file(const char *path) {
    std::ifstream in(path, std::ios::binary);
    assert(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>());
}
static EVP_PKEY *generate_key(std::array<std::uint8_t, 32>& public_key) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    assert(ctx != nullptr && EVP_PKEY_keygen_init(ctx) == 1);
    EVP_PKEY *key = nullptr;
    assert(EVP_PKEY_keygen(ctx, &key) == 1);
    std::size_t size = public_key.size();
    assert(EVP_PKEY_get_raw_public_key(key, public_key.data(), &size) == 1 &&
           size == public_key.size());
    EVP_PKEY_CTX_free(ctx);
    return key;
}
static std::vector<std::uint8_t> descriptor(const std::vector<std::uint8_t>& artifact,
                                            const std::string& allow_read) {
    const std::string magic = ATTO_FS_CEILING_MAGIC;
    std::vector<std::uint8_t> result(magic.begin(), magic.end());
    std::uint8_t digest[32];
    unsigned int digest_size = 0;
    assert(EVP_Digest(artifact.data(), artifact.size(), digest, &digest_size,
                      EVP_sha256(), nullptr) == 1 && digest_size == sizeof(digest));
    result.insert(result.end(), digest, digest + sizeof(digest));
    result.push_back(0xD1u);
    const std::string fields[4] = {allow_read, "out", "data/private", "out/private"};
    for (const auto& field : fields) {
        const auto size = field.size();
        assert(size < ATTO_FS_SIGNED_SCOPE_LIMIT);
        result.push_back(static_cast<std::uint8_t>(size & 0xffu));
        result.push_back(static_cast<std::uint8_t>(size >> 8u));
        result.insert(result.end(), field.begin(), field.end());
    }
    return result;
}
static std::array<std::uint8_t, 64> sign(EVP_PKEY *key,
                                          const std::vector<std::uint8_t>& bytes) {
    auto *ctx = EVP_MD_CTX_new();
    assert(ctx != nullptr);
    assert(EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1);
    std::array<std::uint8_t, 64> signature{};
    std::size_t size = signature.size();
    assert(EVP_DigestSign(ctx, signature.data(), &size,
                          bytes.data(), bytes.size()) == 1 &&
           size == signature.size());
    EVP_MD_CTX_free(ctx);
    return signature;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const auto image = read_file(argv[1]);
    const std::string sha = sha256_hex(image.data(), image.size());
    std::array<std::uint8_t, 32> pubkey{}, wrong_key{};
    EVP_PKEY *key = generate_key(pubkey);
    EVP_PKEY *other = generate_key(wrong_key);
    auto wire = descriptor(image, "data");
    auto signature = sign(key, wire);
    SignedFsDescriptor input{wire.data(), wire.size(),
                             signature.data(), signature.size(),
                             pubkey.data(), pubkey.size()};
    auto loaded = load_guest_library_with_signed_fs_ceiling(argv[1], sha, input);
    assert(loaded.guest.error.empty());
    assert(loaded.guest.module != nullptr);
    assert(loaded.signature_verified);
    assert(std::string(loaded.verified_artifact_ceiling.allow_read) == "data");
    assert(std::string(loaded.verified_artifact_ceiling.allow_write) == "out");

    /* This test loads the actual signed C guest shared library, then asks
     * the trusted HOST to create a six-ceiling broker from its verified
     * artifact maximum. The guest itself is ABI v4 and cannot call FS. */
    char temporary[] = "/tmp/atto-signed-fs-loader-XXXXXX";
    assert(mkdtemp(temporary) != nullptr);
    const std::string root = std::string(temporary) + "/root";
    const std::string data = root + "/data";
    const std::string output = root + "/out";
    assert(mkdir(root.c_str(), 0700) == 0);
    assert(mkdir(data.c_str(), 0700) == 0);
    assert(mkdir(output.c_str(), 0700) == 0);
    {
        std::ofstream file(data + "/input.txt", std::ios::binary);
        assert(file.good());
        file << "signed-input";
    }
    TrustedFsExecutionCeilings ceilings{};
    ceilings.tenant_id = 901u;
    ceilings.generation = 7u;
    ceilings.host = {"*", "*", nullptr, nullptr};
    ceilings.runtime = {"data", "out", nullptr, nullptr};
    ceilings.tenant = {"*", "*", nullptr, nullptr};
    ceilings.actor = {"data", "out", nullptr, nullptr};
    ceilings.delegation = {"*", "*", nullptr, nullptr};
    atto_fs_broker broker{};
    assert(admit_signed_loaded_guest_fs_broker(&broker, loaded, root, ceilings) == 0);
    assert(broker.policy_layer_count == ATTO_FS_POLICY_LAYER_COUNT);
    unsigned char buffer[80] = {};
    std::size_t copied = 0;
    assert(atto_fs_broker_read(&broker, 901u, 7u, "data/input.txt",
                               buffer, sizeof(buffer), &copied) == 0);
    assert(copied == 12u && std::memcmp(buffer, "signed-input", copied) == 0);
    assert(atto_fs_broker_write(&broker, 901u, 7u, "out/copy.txt",
                                buffer, copied) == 0);
    assert(atto_fs_broker_write(&broker, 901u, 7u, "data/input.txt",
                                buffer, copied) == -EACCES);
    assert(atto_fs_broker_read(&broker, 901u, 7u, "../outside.txt",
                               buffer, sizeof(buffer), &copied) != 0);
    assert(atto_fs_broker_revoke(&broker, 901u, 7u) == 0);
    assert(atto_fs_broker_write(&broker, 901u, 7u, "out/copy.txt",
                                buffer, 12u) == -EACCES);
    atto_fs_broker_close(&broker);

    auto forged = loaded;
    forged.signature_verified = false;
    assert(admit_signed_loaded_guest_fs_broker(&broker, forged, root, ceilings) == -EACCES);
    assert(!broker.initialized);
    auto unloaded = loaded;
    unloaded.guest.owner.reset();
    assert(admit_signed_loaded_guest_fs_broker(&broker, unloaded, root, ceilings) == -EACCES);
    assert(!broker.initialized);
    ceilings.runtime.allow_read = nullptr;
    assert(admit_signed_loaded_guest_fs_broker(&broker, loaded, root, ceilings) == 0);
    assert(atto_fs_broker_read(&broker, 901u, 7u, "data/input.txt",
                               buffer, sizeof(buffer), &copied) == -EACCES);
    atto_fs_broker_close(&broker);

    loaded.guest.owner.reset();
    loaded.guest.module = nullptr;

    input.trusted_public_key = wrong_key.data();
    auto rejected_key = load_guest_library_with_signed_fs_ceiling(argv[1], sha, input);
    assert(!rejected_key.guest.error.empty() && !rejected_key.guest.module &&
           !rejected_key.signature_verified);
    input.trusted_public_key = pubkey.data();

    signature[11] ^= 1u;
    auto rejected_signature = load_guest_library_with_signed_fs_ceiling(argv[1], sha, input);
    assert(!rejected_signature.guest.error.empty() && !rejected_signature.guest.module &&
           !rejected_signature.signature_verified);
    signature[11] ^= 1u;

    auto rejected_digest = load_guest_library_with_signed_fs_ceiling(argv[1], std::string(64, '0'), input);
    assert(!rejected_digest.guest.error.empty() && !rejected_digest.guest.module &&
           !rejected_digest.signature_verified);

    wire[wire.size()-1] ^= 1u;
    auto rejected_tamper = load_guest_library_with_signed_fs_ceiling(argv[1], sha, input);
    assert(!rejected_tamper.guest.error.empty() && !rejected_tamper.guest.module &&
           !rejected_tamper.signature_verified);
    wire[wire.size()-1] ^= 1u;

    wire = descriptor(image, "../escape");
    signature = sign(key, wire);
    input.wire = wire.data();
    input.wire_size = wire.size();
    auto rejected_scope = load_guest_library_with_signed_fs_ceiling(argv[1], sha, input);
    assert(!rejected_scope.guest.error.empty() && !rejected_scope.guest.module &&
           !rejected_scope.signature_verified);

    input.signature_size = 63u;
    auto rejected_short_signature = load_guest_library_with_signed_fs_ceiling(argv[1], sha, input);
    assert(!rejected_short_signature.guest.error.empty() && !rejected_short_signature.guest.module &&
           !rejected_short_signature.signature_verified);
    EVP_PKEY_free(other);
    EVP_PKEY_free(key);
    std::puts("ATTO_SIGNED_FS_LOADER_SAME_SEALED_IMAGE_HOST_ONLY_PASS");
    return 0;
}
