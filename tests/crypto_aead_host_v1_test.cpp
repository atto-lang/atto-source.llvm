#include "oreslang/runtime/CryptoAeadHostV1.h"
#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

using atto::crypto::AeadAlgorithm;
using atto::crypto::AeadKey;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void exercise(AeadAlgorithm algorithm) {
    auto key = AeadKey::generate(algorithm);
    auto foreign = AeadKey::generate(algorithm);
    const std::vector<std::uint8_t> aad{1, 2, 3};
    const std::vector<std::uint8_t> plaintext{65, 66, 67};
    std::set<std::array<std::uint8_t, 12>> nonces;
    for (int i = 0; i < 128; ++i) {
        auto sealed = key->seal(aad, plaintext);
        require(nonces.insert(sealed.nonce).second, "AEAD nonce reused");
        require(sealed.ciphertext_and_tag.size() == plaintext.size() + 16,
                "AEAD tag was truncated");
        std::vector<std::uint8_t> output;
        require(key->open(sealed.nonce, aad, sealed.ciphertext_and_tag, output)
                && output == plaintext, "AEAD roundtrip failed");
        auto tampered = sealed.ciphertext_and_tag;
        tampered[0] ^= 1;
        require(!key->open(sealed.nonce, aad, tampered, output) && output.empty(),
                "AEAD tampered ciphertext accepted");
        require(!key->open(sealed.nonce, {9}, sealed.ciphertext_and_tag, output)
                && output.empty(), "AEAD wrong AAD accepted");
        require(!foreign->open(sealed.nonce, aad, sealed.ciphertext_and_tag, output)
                && output.empty(), "AEAD wrong key accepted");
        auto nonce = sealed.nonce;
        nonce[11] ^= 1;
        require(!key->open(nonce, aad, sealed.ciphertext_and_tag, output)
                && output.empty(), "AEAD wrong nonce accepted");
    }
    auto empty = key->seal({}, {});
    require(empty.ciphertext_and_tag.size() == 16, "empty AEAD tag size invalid");
    std::vector<std::uint8_t> empty_output;
    require(key->open(empty.nonce, {}, empty.ciphertext_and_tag, empty_output)
            && empty_output.empty(), "empty AEAD roundtrip failed");
    key->close();
    bool rejected = false;
    try { (void) key->seal({}, {}); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "closed AEAD key accepted");
}

static void aes_nist_vector() {
    // NIST SP 800-38D AES-256-GCM: zero key and 96-bit zero nonce,
    // empty plaintext/AAD -> 16-byte tag 530f8afbc74536b9a963b4f1c4cb738b.
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, 12> nonce{};
    std::array<std::uint8_t, 16> tag{};
    const std::array<std::uint8_t, 16> expected{
        0x53,0x0f,0x8a,0xfb,0xc7,0x45,0x36,0xb9,
        0xa9,0x63,0xb4,0xf1,0xc4,0xcb,0x73,0x8b};
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    require(ctx != nullptr, "OpenSSL context unavailable");
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(),
                                 nonce.data()) == 1;
    int length = 0;
    if (ok) ok = EVP_EncryptFinal_ex(ctx, tag.data(), &length) == 1;
    if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
                                    static_cast<int>(tag.size()), tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    require(ok && tag == expected, "NIST AES-256-GCM vector mismatch");
}

int main() {
    static_assert(!std::is_copy_constructible_v<AeadKey>);
    static_assert(!std::is_move_constructible_v<AeadKey>);
    aes_nist_vector();
    exercise(AeadAlgorithm::Aes256Gcm);
    exercise(AeadAlgorithm::Chacha20Poly1305);
    std::cout << "ATTO_CRYPTO_AEAD_OPENSSL_HOST_ONLY_PASS\n";
    return 0;
}
