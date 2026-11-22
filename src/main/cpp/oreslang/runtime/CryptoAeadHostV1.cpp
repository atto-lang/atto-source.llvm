#include "CryptoAeadHostV1.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <stdexcept>

namespace atto::crypto {
namespace {

using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

const EVP_CIPHER* provider_for(AeadAlgorithm algorithm) {
    switch (algorithm) {
        case AeadAlgorithm::Aes256Gcm: return EVP_aes_256_gcm();
        case AeadAlgorithm::Chacha20Poly1305: return EVP_chacha20_poly1305();
    }
    return nullptr;
}

void require_ok(int status, const char* operation) {
    if (status != 1) throw std::runtime_error(operation);
}

Context new_context() {
    auto* raw = EVP_CIPHER_CTX_new();
    if (raw == nullptr) throw std::runtime_error("AEAD host context unavailable");
    return Context(raw, EVP_CIPHER_CTX_free);
}

} // namespace

AeadKey::AeadKey(AeadAlgorithm algorithm,
                 const std::array<std::uint8_t, KEY_BYTES>& material,
                 const std::array<std::uint8_t, 4>& prefix)
        : algorithm_(algorithm), material_(material), prefix_(prefix) {}

std::unique_ptr<AeadKey> AeadKey::generate(AeadAlgorithm algorithm) {
    if (provider_for(algorithm) == nullptr) {
        throw std::invalid_argument("unsupported vetted AEAD algorithm");
    }
    std::array<std::uint8_t, KEY_BYTES> material{};
    std::array<std::uint8_t, 4> prefix{};
    try {
        require_ok(RAND_bytes(material.data(), static_cast<int>(material.size())),
                   "secure AEAD key generation unavailable");
        require_ok(RAND_bytes(prefix.data(), static_cast<int>(prefix.size())),
                   "secure AEAD nonce entropy unavailable");
        auto result = std::unique_ptr<AeadKey>(new AeadKey(algorithm, material, prefix));
        OPENSSL_cleanse(material.data(), material.size());
        OPENSSL_cleanse(prefix.data(), prefix.size());
        return result;
    } catch (...) {
        OPENSSL_cleanse(material.data(), material.size());
        OPENSSL_cleanse(prefix.data(), prefix.size());
        throw;
    }
}

AeadKey::~AeadKey() {
    close();
}

void AeadKey::close() noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    if (closed_) return;
    closed_ = true;
    OPENSSL_cleanse(material_.data(), material_.size());
    OPENSSL_cleanse(prefix_.data(), prefix_.size());
}

std::array<std::uint8_t, AeadKey::NONCE_BYTES> AeadKey::reserve_nonce() {
    if (counter_ >= MAX_INVOCATIONS) {
        throw std::runtime_error("AEAD per-key invocation limit exhausted");
    }
    std::array<std::uint8_t, NONCE_BYTES> nonce{};
    std::copy(prefix_.begin(), prefix_.end(), nonce.begin());
    std::uint64_t value = counter_++;
    for (int index = 11; index >= 4; --index) {
        nonce[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
    return nonce;
}

SealedMessage AeadKey::seal(const std::vector<std::uint8_t>& aad,
                            const std::vector<std::uint8_t>& plaintext) {
    if (aad.size() > MAX_AAD_BYTES || plaintext.size() > MAX_MESSAGE_BYTES) {
        throw std::invalid_argument("AEAD input size exceeds budget");
    }
    std::lock_guard<std::mutex> guard(lock_);
    if (closed_) throw std::runtime_error("AEAD key closed");
    const EVP_CIPHER* cipher = provider_for(algorithm_);
    if (cipher == nullptr) throw std::runtime_error("vetted AEAD unavailable");
    SealedMessage sealed{reserve_nonce(), std::vector<std::uint8_t>(plaintext.size() + TAG_BYTES)};
    try {
        auto ctx = new_context();
        require_ok(EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr),
                   "AEAD encrypt initialization failed");
        require_ok(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN,
                                      static_cast<int>(sealed.nonce.size()), nullptr),
                   "AEAD nonce size unsupported");
        require_ok(EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                                     material_.data(), sealed.nonce.data()),
                   "AEAD key initialization failed");
        int ignored = 0;
        if (!aad.empty()) require_ok(
            EVP_EncryptUpdate(ctx.get(), nullptr, &ignored, aad.data(),
                              static_cast<int>(aad.size())),
            "AEAD additional data processing failed");
        int encrypted = 0;
        if (!plaintext.empty()) require_ok(
            EVP_EncryptUpdate(ctx.get(), sealed.ciphertext_and_tag.data(), &encrypted,
                              plaintext.data(), static_cast<int>(plaintext.size())),
            "AEAD ciphertext generation failed");
        int tail = 0;
        require_ok(EVP_EncryptFinal_ex(ctx.get(),
                    sealed.ciphertext_and_tag.data() + encrypted, &tail),
                   "AEAD encryption finalization failed");
        if (static_cast<std::size_t>(encrypted + tail) != plaintext.size()) {
            throw std::runtime_error("unexpected AEAD ciphertext size");
        }
        require_ok(EVP_CIPHER_CTX_ctrl(
                       ctx.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(TAG_BYTES),
                       sealed.ciphertext_and_tag.data() + plaintext.size()),
                   "AEAD full authentication tag unavailable");
        return sealed;
    } catch (...) {
        OPENSSL_cleanse(sealed.ciphertext_and_tag.data(), sealed.ciphertext_and_tag.size());
        throw;
    }
}

bool AeadKey::open(const std::array<std::uint8_t, NONCE_BYTES>& nonce,
                    const std::vector<std::uint8_t>& aad,
                    const std::vector<std::uint8_t>& ciphertext_and_tag,
                    std::vector<std::uint8_t>& output) {
    output.clear();
    if (aad.size() > MAX_AAD_BYTES ||
        ciphertext_and_tag.size() < TAG_BYTES ||
        ciphertext_and_tag.size() > MAX_MESSAGE_BYTES + TAG_BYTES) {
        return false;
    }
    std::lock_guard<std::mutex> guard(lock_);
    if (closed_) throw std::runtime_error("AEAD key closed");
    const EVP_CIPHER* cipher = provider_for(algorithm_);
    if (cipher == nullptr) throw std::runtime_error("vetted AEAD unavailable");
    const std::size_t plaintext_length = ciphertext_and_tag.size() - TAG_BYTES;
    std::vector<std::uint8_t> unverified(plaintext_length);
    std::array<std::uint8_t, TAG_BYTES> dummy{};
    try {
        auto ctx = new_context();
        require_ok(EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr),
                   "AEAD decrypt initialization failed");
        require_ok(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN,
                                      static_cast<int>(nonce.size()), nullptr),
                   "AEAD nonce size unsupported");
        require_ok(EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                                     material_.data(), nonce.data()),
                   "AEAD key initialization failed");
        int ignored = 0;
        if (!aad.empty()) require_ok(
            EVP_DecryptUpdate(ctx.get(), nullptr, &ignored,
                              aad.data(), static_cast<int>(aad.size())),
            "AEAD additional data processing failed");
        int decrypted = 0;
        if (plaintext_length != 0) require_ok(
            EVP_DecryptUpdate(ctx.get(), unverified.data(), &decrypted,
                              ciphertext_and_tag.data(), static_cast<int>(plaintext_length)),
            "AEAD decrypt processing failed");
        auto* tag = const_cast<std::uint8_t*>(
            ciphertext_and_tag.data() + plaintext_length);
        require_ok(EVP_CIPHER_CTX_ctrl(
                       ctx.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(TAG_BYTES), tag),
                   "AEAD full tag verification unavailable");
        int tail = 0;
        auto* final_destination = plaintext_length == 0
            ? dummy.data() : unverified.data() + decrypted;
        const int verified = EVP_DecryptFinal_ex(ctx.get(), final_destination, &tail);
        if (verified != 1 || static_cast<std::size_t>(decrypted + tail) != plaintext_length) {
            if (!unverified.empty()) OPENSSL_cleanse(unverified.data(), unverified.size());
            return false;
        }
        output.swap(unverified);
        return true;
    } catch (...) {
        if (!unverified.empty()) OPENSSL_cleanse(unverified.data(), unverified.size());
        throw;
    }
}
} // namespace atto::crypto
