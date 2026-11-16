#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace oreslang::runtime {

struct ActorIoAuthority final {
    std::uint64_t actor_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t nonce = 0;

    friend bool operator==(const ActorIoAuthority& a,
                           const ActorIoAuthority& b) noexcept {
        return a.actor_id == b.actor_id &&
               a.generation == b.generation &&
               a.nonce == b.nonce;
    }
    friend bool operator!=(const ActorIoAuthority& a,
                           const ActorIoAuthority& b) noexcept {
        return !(a == b);
    }
};

template <std::size_t MaxRecords, std::size_t MaxRecordBytes>
class ActorIoByteQueue final {
    static_assert(MaxRecords > 0, "actor I/O record capacity must be positive");
    static_assert(MaxRecordBytes > 0, "actor I/O byte capacity must be positive");

    struct Slot final {
        std::array<std::uint8_t, MaxRecordBytes> bytes{};
        std::size_t length = 0;
    };

public:
    bool try_push(const void* data, std::size_t length) noexcept {
        if (length > MaxRecordBytes || count_ == MaxRecords) return false;
        if (length != 0 && data == nullptr) return false;
        Slot& slot = slots_[tail_];
        std::fill(slot.bytes.begin(), slot.bytes.end(), 0);
        if (length != 0)
            std::memcpy(slot.bytes.data(), data, length);
        slot.length = length;
        tail_ = (tail_ + 1) % MaxRecords;
        ++count_;
        return true;
    }

    bool try_pop(void* out, std::size_t out_capacity,
                 std::size_t& out_length) noexcept {
        if (count_ == 0) {
            out_length = 0;
            return false;
        }
        Slot& slot = slots_[head_];
        if (slot.length > out_capacity || (slot.length != 0 && out == nullptr))
            return false;
        if (slot.length != 0)
            std::memcpy(out, slot.bytes.data(), slot.length);
        out_length = slot.length;
        std::fill(slot.bytes.begin(), slot.bytes.end(), 0);
        slot.length = 0;
        head_ = (head_ + 1) % MaxRecords;
        --count_;
        return true;
    }

    std::size_t size() const noexcept { return count_; }

    void clear() noexcept {
        for (auto& slot : slots_) {
            std::fill(slot.bytes.begin(), slot.bytes.end(), 0);
            slot.length = 0;
        }
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

private:
    std::array<Slot, MaxRecords> slots_{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
};

template <std::size_t MaxTextBytes>
struct ActorIoTextPair final {
    std::array<char, MaxTextBytes> first{};
    std::array<char, MaxTextBytes> second{};
    std::size_t first_length = 0;
    std::size_t second_length = 0;

    std::string_view first_view() const noexcept {
        return std::string_view(first.data(), first_length);
    }

    std::string_view second_view() const noexcept {
        return std::string_view(second.data(), second_length);
    }

    void clear() noexcept {
        std::fill(first.begin(), first.end(), '\0');
        std::fill(second.begin(), second.end(), '\0');
        first_length = 0;
        second_length = 0;
    }
};

template <std::size_t MaxRecords, std::size_t MaxTextBytes>
class ActorIoPairQueue final {
    static_assert(MaxRecords > 0, "actor I/O pair capacity must be positive");
    static_assert(MaxTextBytes > 0, "actor I/O text capacity must be positive");

    using Record = ActorIoTextPair<MaxTextBytes>;

public:
    bool try_push(std::string_view first, std::string_view second) noexcept {
        if (count_ == MaxRecords ||
            first.size() > MaxTextBytes ||
            second.size() > MaxTextBytes)
            return false;
        Record& slot = slots_[tail_];
        slot.clear();
        std::copy(first.begin(), first.end(), slot.first.begin());
        std::copy(second.begin(), second.end(), slot.second.begin());
        slot.first_length = first.size();
        slot.second_length = second.size();
        tail_ = (tail_ + 1) % MaxRecords;
        ++count_;
        return true;
    }

    bool try_pop(Record& out) noexcept {
        if (count_ == 0) {
            out.clear();
            return false;
        }
        Record& slot = slots_[head_];
        out = slot;
        slot.clear();
        head_ = (head_ + 1) % MaxRecords;
        --count_;
        return true;
    }

    void clear() noexcept {
        for (auto& slot : slots_) slot.clear();
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

private:
    std::array<Record, MaxRecords> slots_{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
};

template <
    std::size_t MaxRecords = 64,
    std::size_t MaxRecordBytes = 4096,
    std::size_t MaxTextBytes = 256,
    std::size_t MaxMetadataEntries = 16,
    std::size_t MaxMetadataKeyBytes = 32,
    std::size_t MaxMetadataValueBytes = 128>
class NativeActorIoEnvelope final {
    static_assert(MaxMetadataEntries > 0, "metadata capacity must be positive");
    static_assert(MaxMetadataKeyBytes > 0, "metadata key capacity must be positive");
    static_assert(MaxMetadataValueBytes > 0, "metadata value capacity must be positive");

    struct MetadataSlot final {
        std::array<char, MaxMetadataKeyBytes> key{};
        std::array<char, MaxMetadataValueBytes> value{};
        std::size_t key_length = 0;
        std::size_t value_length = 0;
        bool used = false;
    };

public:
    explicit NativeActorIoEnvelope(ActorIoAuthority authority)
        : authority_(authority) {
        validate_authority(authority_);
    }

    const ActorIoAuthority& authority() const noexcept { return authority_; }

    bool try_feed_input(const ActorIoAuthority& authority,
                        const void* bytes, std::size_t length) {
        require(authority);
        return input_.try_push(bytes, length);
    }

    bool try_read_input(const ActorIoAuthority& authority,
                        void* out, std::size_t capacity,
                        std::size_t& length) {
        require(authority);
        return input_.try_pop(out, capacity, length);
    }

    bool try_write_output(const ActorIoAuthority& authority,
                          const void* bytes, std::size_t length) {
        require(authority);
        return output_.try_push(bytes, length);
    }

    bool try_read_output(const ActorIoAuthority& authority,
                         void* out, std::size_t capacity,
                         std::size_t& length) {
        require(authority);
        return output_.try_pop(out, capacity, length);
    }

    bool try_emit_error(const ActorIoAuthority& authority,
                        std::string_view code,
                        std::string_view message) {
        require(authority);
        return errors_.try_push(code, message);
    }

    bool try_read_error(const ActorIoAuthority& authority,
                        ActorIoTextPair<MaxTextBytes>& diagnostic) {
        require(authority);
        return errors_.try_pop(diagnostic);
    }

    bool try_emit_event(const ActorIoAuthority& authority,
                        std::string_view kind,
                        std::string_view payload) {
        require(authority);
        return events_.try_push(kind, payload);
    }

    bool try_read_event(const ActorIoAuthority& authority,
                        ActorIoTextPair<MaxTextBytes>& event) {
        require(authority);
        return events_.try_pop(event);
    }

    std::uint64_t metadata_version(const ActorIoAuthority& authority) const {
        require(authority);
        return metadata_version_;
    }

    bool replace_metadata(
        const ActorIoAuthority& authority,
        const std::array<std::pair<std::string_view, std::string_view>,
                         MaxMetadataEntries>& entries,
        std::size_t count) {
        require(authority);
        if (count > MaxMetadataEntries) return false;
        std::array<MetadataSlot, MaxMetadataEntries> replacement{};
        for (std::size_t i = 0; i < count; ++i) {
            const auto key = entries[i].first;
            const auto value = entries[i].second;
            if (key.empty() ||
                key.size() > MaxMetadataKeyBytes ||
                value.size() > MaxMetadataValueBytes)
                return false;
            for (std::size_t j = 0; j < i; ++j) {
                if (replacement[j].used &&
                    std::string_view(replacement[j].key.data(),
                                     replacement[j].key_length) == key)
                    return false;
            }
            auto& slot = replacement[i];
            std::copy(key.begin(), key.end(), slot.key.begin());
            std::copy(value.begin(), value.end(), slot.value.begin());
            slot.key_length = key.size();
            slot.value_length = value.size();
            slot.used = true;
        }
        if (metadata_version_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor I/O metadata version exhausted");
        clear_metadata();
        metadata_ = replacement;
        ++metadata_version_;
        return true;
    }

    std::optional<std::string_view> metadata_value(
        const ActorIoAuthority& authority,
        std::string_view key) const {
        require(authority);
        for (const auto& slot : metadata_) {
            if (!slot.used) continue;
            if (std::string_view(slot.key.data(), slot.key_length) == key)
                return std::string_view(slot.value.data(), slot.value_length);
        }
        return std::nullopt;
    }

    ActorIoAuthority restart(std::uint64_t new_generation,
                             std::uint64_t new_nonce) {
        if (new_generation <= authority_.generation || new_nonce == 0 ||
            new_nonce == authority_.nonce)
            throw std::invalid_argument(
                "actor I/O restart requires fresh generation and nonce");
        input_.clear();
        output_.clear();
        errors_.clear();
        events_.clear();
        clear_metadata();
        metadata_version_ = 1;
        authority_.generation = new_generation;
        authority_.nonce = new_nonce;
        return authority_;
    }

private:
    static void validate_authority(const ActorIoAuthority& authority) {
        if (authority.actor_id == 0 ||
            authority.generation == 0 ||
            authority.nonce == 0)
            throw std::invalid_argument(
                "actor I/O authority fields must be nonzero");
    }

    void require(const ActorIoAuthority& authority) const {
        if (authority != authority_)
            throw std::invalid_argument(
                "actor I/O authority is stale, foreign, or retired");
    }

    void clear_metadata() noexcept {
        for (auto& slot : metadata_) {
            std::fill(slot.key.begin(), slot.key.end(), '\0');
            std::fill(slot.value.begin(), slot.value.end(), '\0');
            slot.key_length = 0;
            slot.value_length = 0;
            slot.used = false;
        }
    }

    ActorIoAuthority authority_;
    ActorIoByteQueue<MaxRecords, MaxRecordBytes> input_;
    ActorIoByteQueue<MaxRecords, MaxRecordBytes> output_;
    ActorIoPairQueue<MaxRecords, MaxTextBytes> errors_;
    ActorIoPairQueue<MaxRecords, MaxTextBytes> events_;
    std::array<MetadataSlot, MaxMetadataEntries> metadata_{};
    std::uint64_t metadata_version_ = 1;
};

} // namespace oreslang::runtime
