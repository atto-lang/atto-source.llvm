#pragma once
// Bounded ODB1 v1 WAL replay scanner, host-only. No guest ABI, checkpoint,
// truncation, durable ACK or state publication. The caller MUST own an
// exclusive trusted recovery file lease and stage results until the entire
// scan succeeds. Never expose a partial visitor application as committed.
#include "oreslang/runtime/DbWalFrameV1.hpp"
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace oreslang::runtime {

class DbWalReplayV1 final {
public:
    struct Report {
        std::uint64_t valid_bytes = 0;
        std::uint32_t records = 0;
        std::uint32_t last_sequence = 0;
        std::uint32_t last_operation = 0;
        bool incomplete_tail = false;
    };

    // A single-generation segment. Reject non-monotonic/duplicate op IDs
    // and non-contiguous sequences. No checkpoint or multi-generation replay.
    // An incomplete final header/frame is reported without truncation; a
    // complete corrupt record is ALWAYS fatal, even when it is the last.
    template<class Visitor>
    static Report scan(int fd, std::uint32_t tenant,
            std::uint32_t generation, Visitor&& visitor) {
        if (tenant == 0 || generation == 0 || tenant > 0x7fffffffU
                || generation > 0x7fffffffU)
            throw std::invalid_argument("WAL recovery owner invalid");
        struct stat info{};
        if (::fstat(fd, &info) != 0)
            throw std::system_error(errno, std::generic_category(), "WAL replay fstat");
        if (!S_ISREG(info.st_mode) || info.st_size < 0)
            throw std::invalid_argument("WAL recovery requires regular file");
        const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
        if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            throw std::overflow_error("WAL recovery file offset overflow");
        Report result{};
        std::array<std::uint8_t, 1118> bytes{};
        while (result.valid_bytes < size) {
            const std::uint64_t remaining = size - result.valid_bytes;
            if (remaining < 26) {
                // A truncated tail must still match all known header bytes.
                read_exact(fd, bytes.data(),
                        static_cast<std::size_t>(remaining),
                        result.valid_bytes);
                validate_header_prefix(bytes.data(),
                        static_cast<std::size_t>(remaining), tenant,
                        generation, result.last_sequence,
                        result.last_operation);
                result.incomplete_tail = true;
                break;
            }
            read_exact(fd, bytes.data(), 26, result.valid_bytes);
            validate_header_prefix(bytes.data(), 26, tenant,
                    generation, result.last_sequence,
                    result.last_operation);
            if (bytes[0] != 79 || bytes[1] != 68 || bytes[2] != 66
                    || bytes[3] != 49 || bytes[4] != 1
                    || (bytes[5] != 1 && bytes[5] != 2))
                throw std::invalid_argument("WAL corrupt full header");
            const std::uint32_t key_size = u16(bytes.data(), 22);
            const std::uint32_t value_size = u16(bytes.data(), 24);
            if (key_size == 0 || key_size > 64 || value_size > 1024
                    || (bytes[5] == 2 && value_size != 0))
                throw std::invalid_argument("WAL invalid replay length");
            const std::size_t frame_size = 26U + key_size + value_size + 4U;
            if (frame_size > remaining) {
                result.incomplete_tail = true;
                break;
            }
            read_exact(fd, bytes.data() + 26, frame_size - 26,
                    result.valid_bytes + 26);
            const std::uint32_t op = u32(bytes.data(), 18);
            DbWalFrameV1::require_matching(
                    bytes.data(), frame_size, tenant, generation, op);
            const std::uint32_t seq = u32(bytes.data(), 14);
            if (result.last_sequence == 0) {
                if (seq != 1) throw std::invalid_argument(
                        "WAL replay initial sequence not 1");
            } else if (result.last_sequence == 0x7fffffffU
                    || seq != result.last_sequence + 1)
                throw std::invalid_argument("WAL replay sequence gap/reorder");
            if (op <= result.last_operation)
                throw std::invalid_argument("WAL replay duplicate/reordered op");
            if (result.records == 0x7fffffffU)
                throw std::overflow_error("WAL replay record count");
            // Visitor MUST stage results transactionally; a later invalid
            // record invalidates the entire scan. No public ACK is issued.
            visitor(bytes.data(), frame_size, result.valid_bytes);
            result.valid_bytes += frame_size;
            ++result.records;
            result.last_sequence = seq;
            result.last_operation = op;
        }
        return result;
    }
private:
    static void require_field_prefix(const std::uint8_t* bytes,
            std::size_t length, std::size_t offset,
            std::uint32_t expected) {
        for (std::size_t i = 0; i < 4 && offset + i < length; ++i) {
            if (bytes[offset + i] !=
                    static_cast<std::uint8_t>(expected >> (8U * i)))
                throw std::invalid_argument(
                    "WAL truncated identity/sequence prefix mismatch");
        }
    }

    // Reject malformed short or full headers BEFORE marking a torn tail.
    static void validate_header_prefix(const std::uint8_t* bytes,
            std::size_t length, std::uint32_t tenant,
            std::uint32_t generation, std::uint32_t previous_sequence,
            std::uint32_t previous_operation) {
        const std::uint8_t magic[] = {79, 68, 66, 49, 1};
        for (std::size_t i = 0; i < length && i < 5; ++i)
            if (bytes[i] != magic[i])
                throw std::invalid_argument("WAL truncated magic/version");
        if (length > 5 && bytes[5] != 1 && bytes[5] != 2)
            throw std::invalid_argument("WAL truncated kind invalid");
        if (previous_sequence == 0x7fffffffU)
            throw std::invalid_argument("WAL sequence exhausted");
        require_field_prefix(bytes, length, 6, tenant);
        require_field_prefix(bytes, length, 10, generation);
        require_field_prefix(bytes, length, 14, previous_sequence + 1U);
        if (length >= 22 &&
                ((bytes[21] & 128U) != 0U ||
                    u32(bytes, 18) <= previous_operation))
            throw std::invalid_argument("WAL operation not increasing");
        if (length >= 24) {
            const auto key_length = u16(bytes, 22);
            if (key_length == 0 || key_length > 64)
                throw std::invalid_argument("WAL truncated key size");
        }
        if (length >= 26) {
            const auto value_length = u16(bytes, 24);
            if (value_length > 1024 ||
                    (bytes[5] == 2 && value_length != 0))
                throw std::invalid_argument("WAL truncated value size");
        }
    }

    static std::uint32_t u16(const std::uint8_t* p, std::size_t at) noexcept {
        return std::uint32_t(p[at]) | (std::uint32_t(p[at + 1]) << 8);
    }
    static std::uint32_t u32(const std::uint8_t* p, std::size_t at) noexcept {
        return u16(p, at) | (u16(p, at + 2) << 16);
    }
    static void read_exact(int fd, std::uint8_t* into, std::size_t bytes,
            std::uint64_t offset) {
        std::size_t done = 0;
        while (done < bytes) {
            const auto pos = static_cast<off_t>(offset + done);
            const ssize_t n = ::pread(fd, into + done, bytes - done, pos);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) throw std::system_error(
                    errno, std::generic_category(), "WAL replay pread");
            if (n == 0) throw std::runtime_error(
                    "WAL changed during recovery; abort without publishing");
            done += static_cast<std::size_t>(n);
        }
    }
};

} // namespace oreslang::runtime
