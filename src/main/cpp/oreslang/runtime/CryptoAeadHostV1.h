#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace atto::crypto {

// Host-only OpenSSL AEAD. The compiler/actor runtime must separately verify
// capability, context/actor incarnation, ownership and memory permissions.
enum class AeadAlgorithm : int { Aes256Gcm = 1, Chacha20Poly1305 = 2 };

struct SealedMessage {
    std::array<std::uint8_t, 12> nonce;
    // Payload includes the full 16-byte authentication tag.
    std::vector<std::uint8_t> ciphertext_and_tag;
};

class AeadKey final {
public:
    static constexpr std::size_t KEY_BYTES = 32;
    static constexpr std::size_t NONCE_BYTES = 12;
    static constexpr std::size_t TAG_BYTES = 16;
    static constexpr std::size_t MAX_MESSAGE_BYTES = 16U * 1024U * 1024U;
    static constexpr std::size_t MAX_AAD_BYTES = 64U * 1024U;
    static constexpr std::uint64_t MAX_INVOCATIONS = UINT64_C(1) << 32;

    static std::unique_ptr<AeadKey> generate(AeadAlgorithm algorithm);

    AeadKey(const AeadKey&) = delete;
    AeadKey& operator=(const AeadKey&) = delete;
    AeadKey(AeadKey&&) = delete;
    AeadKey& operator=(AeadKey&&) = delete;
    ~AeadKey();

    SealedMessage seal(const std::vector<std::uint8_t>& aad,
                       const std::vector<std::uint8_t>& plaintext);

    // False on bad nonce, AAD, ciphertext length or authentication tag.
    // output is not published until the whole tag authenticates.
    bool open(const std::array<std::uint8_t, NONCE_BYTES>& nonce,
              const std::vector<std::uint8_t>& aad,
              const std::vector<std::uint8_t>& ciphertext_and_tag,
              std::vector<std::uint8_t>& output);

    void close() noexcept;

private:
    AeadKey(AeadAlgorithm algorithm,
            const std::array<std::uint8_t, KEY_BYTES>& material,
            const std::array<std::uint8_t, 4>& prefix);
    std::array<std::uint8_t, NONCE_BYTES> reserve_nonce();

    const AeadAlgorithm algorithm_;
    std::array<std::uint8_t, KEY_BYTES> material_;
    std::array<std::uint8_t, 4> prefix_;
    std::uint64_t counter_ = 0;
    bool closed_ = false;
    std::mutex lock_;
};
} // namespace atto::crypto
