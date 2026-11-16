#pragma once

#if !defined(__linux__)
#error "NativeIpcAuthenticatedStreamSession is currently Linux-only"
#endif

#include "oreslang/runtime/NativeIpcReceiptTable.hpp"
#include "oreslang/runtime/NativeIpcWireV3.hpp"
#include "oreslang/runtime/NonblockingTcpLinux.hpp"

#include <openssl/crypto.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Trusted host-side protocol-v3 stream session for one physical connection.
 *
 * The socket is O_NONBLOCK. No method parks on socket readiness: callers use a
 * dedicated reactor and invoke send/flush/poll when EPOLLOUT/EPOLLIN says the
 * descriptor may make progress. The actor carrier must never call epoll_wait.
 *
 * One DATA frame may be physically publishing at a time. Receipt lifetimes are
 * bounded independently by the private correlation table.
 */
class NativeIpcAuthenticatedStreamSession final {
    class SensitiveKey final {
    public:
        explicit SensitiveKey(const NativeIpcWireV3::Bytes& source)
            : bytes(source) { }
        SensitiveKey(const SensitiveKey&) = delete;
        SensitiveKey& operator=(const SensitiveKey&) = delete;
        ~SensitiveKey() noexcept { cleanse(); }

        void cleanse() noexcept {
            if (!bytes.empty())
                OPENSSL_cleanse(bytes.data(), bytes.size());
        }

        NativeIpcWireV3::Bytes bytes;
    };

public:
    enum class Phase : std::uint8_t { open, retired, quarantined };

    enum class SendStatus : std::uint8_t {
        complete,
        would_block,
        busy,
        retired
    };

    explicit NativeIpcAuthenticatedStreamSession(
            OwnedSocketFd socket,
            IpcReceiptEnvelope trusted_epoch,
            std::uint64_t physical_generation,
            std::size_t receipt_capacity,
            const NativeIpcWireV3::Bytes& mac_key,
            NativeIpcWireV3::ConnectionBinding connection_binding)
        : socket_(std::move(socket)),
          trusted_epoch_(std::move(trusted_epoch)),
          physical_generation_(physical_generation),
          receipts_(receipt_capacity),
          mac_key_(mac_key),
          connection_binding_(connection_binding) {
        if (!socket_.valid())
            throw std::invalid_argument("IPC session requires an owned socket");
        validate_ipc_receipt_envelope(trusted_epoch_);
        if (!ipc_positive_java_long(physical_generation_))
            throw std::invalid_argument(
                "IPC physical generation must be a positive Java long");
        // Force codec validation before the session becomes usable.
        (void)NativeIpcWireV3::encode_receipt_frame(
            trusted_epoch_, physical_generation_,
            IpcReceiptStage::remote_accepted,
            connection_binding_, mac_key_.bytes);
    }

    ~NativeIpcAuthenticatedStreamSession() noexcept {
        socket_.reset();
        cleanse_key();
    }

    NativeIpcAuthenticatedStreamSession(
        const NativeIpcAuthenticatedStreamSession&) = delete;
    NativeIpcAuthenticatedStreamSession& operator=(
        const NativeIpcAuthenticatedStreamSession&) = delete;

    /**
     * Encode entirely before receipt admission. If admitted, dispatch commit
     * and the first nonblocking kernel write attempt happen in this same call
     * before it can return, so there is no abandonable committed-write token.
     */
    SendStatus send_nonblocking(
            const IpcReceiptEnvelope& message,
            const NativeIpcWireV3::Bytes& payload) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::open)
            return SendStatus::retired;
        if (!same_ipc_transport_epoch(message, trusted_epoch_))
            throw std::invalid_argument(
                "message does not match authenticated transport epoch");
        if (!outbound_.empty())
            return SendStatus::busy;

        // Encode and allocate every byte before receipt-table mutation while
        // the same gate prevents concurrent key destruction.
        const auto authenticated = NativeIpcWireV3::encode_data_frame(
            message, payload, physical_generation_,
            connection_binding_, mac_key_.bytes);
        auto wire = prefix(authenticated);

        receipts_.admit(message);
        try {
            receipts_.commit_dispatch(message);
        } catch (...) {
            receipts_.reject_before_remote(message);
            throw;
        }

        outbound_.swap(wire);
        outbound_offset_ = 0;
        return flush_locked();
    }

    /** Continue only a previously attempted, dispatch-committed write. */
    SendStatus flush_nonblocking() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::open)
            return SendStatus::retired;
        if (outbound_.empty())
            return SendStatus::complete;
        return flush_locked();
    }

    /**
     * Consume at most one authenticated receipt. Length-prefix admission is
     * bounded before allocating a frame vector. Decode + epoch/generation
     * validation + ledger application stay under one receive gate (this
     * session mutex), preserving TCP receipt order with concurrent host callers.
     */
    std::optional<IpcReceiptStage> poll_receipt_nonblocking() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::open)
            return std::nullopt;

        try {
            if (!fill_receipt_locked())
                return std::nullopt;

            const std::size_t frame_size = receipt_target_ - prefix_bytes;
            NativeIpcWireV3::Bytes frame(
                receipt_buffer_.begin() + static_cast<std::ptrdiff_t>(prefix_bytes),
                receipt_buffer_.begin() + static_cast<std::ptrdiff_t>(receipt_target_));
            receipt_size_ = 0;
            receipt_target_ = 0;

            if (frame.size() != frame_size)
                throw std::logic_error("IPC receipt framing size mismatch");

            const auto decoded = NativeIpcWireV3::decode_receipt_frame(
                frame, connection_binding_, mac_key_.bytes);
            if (decoded.physical_generation != physical_generation_)
                throw std::invalid_argument(
                    "stale IPC physical transport generation");
            if (!same_ipc_transport_epoch(
                    decoded.envelope, trusted_epoch_))
                throw std::invalid_argument(
                    "stale IPC receipt transport epoch");
            receipts_.accept_verified(decoded.envelope, decoded.stage);
            return decoded.stage;
        } catch (...) {
            fail_closed_locked();
            throw;
        }
    }

    /**
     * Trusted local close. Physical close happens before correlation loss is
     * published. Allocation failure during loss materialization quarantines
     * the generation; explicit retry is required and never reopens the socket.
     */
    void close_and_retire() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ == Phase::retired)
            return;
        if (phase_ == Phase::quarantined) {
            retry_retire_locked();
            return;
        }
        fail_closed_locked();
    }

    void retry_quarantined_retire() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::quarantined)
            throw std::logic_error("IPC session is not quarantined");
        retry_retire_locked();
    }

    std::vector<NativeIpcReceiptTable::LostCorrelation> take_losses() {
        std::lock_guard<std::mutex> guard(mutex_);
        auto result = std::move(losses_);
        losses_.clear();
        return result;
    }

    Phase phase() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return phase_;
    }

    bool has_pending_write() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return !outbound_.empty();
    }

    bool invariant() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return receipts_.invariant() &&
            outbound_offset_ <= outbound_.size() &&
            receipt_size_ <= receipt_buffer_.size() &&
            (receipt_target_ == 0 ||
             (receipt_target_ >= prefix_bytes + NativeIpcWireV3::mac_bytes &&
              receipt_target_ <= receipt_buffer_.size())) &&
            (phase_ == Phase::open || !socket_.valid());
    }

    std::pair<std::size_t, std::size_t> counters() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return receipts_.counters();
    }

    bool key_zeroized() const {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto byte : mac_key_.bytes)
            if (byte != 0u) return false;
        return true;
    }

private:
    static constexpr std::size_t prefix_bytes = 4u;

    static NativeIpcWireV3::Bytes prefix(
            const NativeIpcWireV3::Bytes& frame) {
        if (frame.size() > NativeIpcWireV3::max_data_frame_bytes)
            throw std::invalid_argument("IPC DATA frame exceeds bound");
        NativeIpcWireV3::Bytes wire;
        wire.reserve(prefix_bytes + frame.size());
        const auto size = static_cast<std::uint32_t>(frame.size());
        wire.push_back(static_cast<unsigned char>((size >> 24u) & 0xffu));
        wire.push_back(static_cast<unsigned char>((size >> 16u) & 0xffu));
        wire.push_back(static_cast<unsigned char>((size >> 8u) & 0xffu));
        wire.push_back(static_cast<unsigned char>(size & 0xffu));
        wire.insert(wire.end(), frame.begin(), frame.end());
        return wire;
    }

    static std::uint32_t parse_length(
            const std::array<unsigned char,
                prefix_bytes + NativeIpcWireV3::max_receipt_frame_bytes>& bytes) {
        return (static_cast<std::uint32_t>(bytes[0]) << 24u) |
               (static_cast<std::uint32_t>(bytes[1]) << 16u) |
               (static_cast<std::uint32_t>(bytes[2]) << 8u) |
               static_cast<std::uint32_t>(bytes[3]);
    }

    SendStatus flush_locked() {
        if (outbound_.empty())
            return SendStatus::complete;

        const auto attempt = try_write(
            socket_.get(), outbound_.data() + outbound_offset_,
            outbound_.size() - outbound_offset_);
        if (attempt.disposition == IoDisposition::transferred) {
            outbound_offset_ += attempt.size;
            if (outbound_offset_ == outbound_.size()) {
                outbound_.clear();
                outbound_offset_ = 0;
                return SendStatus::complete;
            }
            return SendStatus::would_block;
        }
        if (attempt.disposition == IoDisposition::would_block)
            return SendStatus::would_block;

        const int error_number = attempt.error_number == 0
            ? EPIPE : attempt.error_number;
        fail_closed_locked();
        throw std::system_error(
            error_number, std::generic_category(),
            "authenticated IPC DATA write failed");
    }

    bool fill_receipt_locked() {
        for (;;) {
            const std::size_t target = receipt_target_ == 0
                ? prefix_bytes : receipt_target_;
            if (receipt_size_ >= target) {
                if (receipt_target_ == 0) {
                    const auto length = parse_length(receipt_buffer_);
                    if (length < NativeIpcWireV3::mac_bytes ||
                        length > NativeIpcWireV3::max_receipt_frame_bytes)
                        throw std::invalid_argument(
                            "invalid bounded IPC receipt wire length");
                    receipt_target_ = prefix_bytes +
                        static_cast<std::size_t>(length);
                    continue;
                }
                return true;
            }

            const auto attempt = try_read(
                socket_.get(), receipt_buffer_.data() + receipt_size_,
                target - receipt_size_);
            if (attempt.disposition == IoDisposition::transferred) {
                receipt_size_ += attempt.size;
                continue;
            }
            if (attempt.disposition == IoDisposition::would_block)
                return false;
            if (attempt.disposition == IoDisposition::eof) {
                fail_closed_locked();
                return false;
            }

            const int error_number = attempt.error_number == 0
                ? EIO : attempt.error_number;
            fail_closed_locked();
            throw std::system_error(
                error_number, std::generic_category(),
                "authenticated IPC receipt read failed");
        }
    }

    void fail_closed_locked() {
        if (phase_ == Phase::retired)
            return;
        socket_.reset();
        outbound_.clear();
        outbound_offset_ = 0;
        receipt_size_ = 0;
        receipt_target_ = 0;
        cleanse_key();

        try {
            losses_ = receipts_.retire_transport_lost(trusted_epoch_);
            phase_ = Phase::retired;
        } catch (...) {
            phase_ = Phase::quarantined;
            throw;
        }
    }

    void retry_retire_locked() {
        try {
            losses_ = receipts_.retire_transport_lost(trusted_epoch_);
            phase_ = Phase::retired;
        } catch (...) {
            phase_ = Phase::quarantined;
            throw;
        }
    }

    void cleanse_key() noexcept {
        mac_key_.cleanse();
    }

    mutable std::mutex mutex_;
    OwnedSocketFd socket_;
    const IpcReceiptEnvelope trusted_epoch_;
    const std::uint64_t physical_generation_;
    NativeIpcReceiptTable receipts_;
    SensitiveKey mac_key_;
    const NativeIpcWireV3::ConnectionBinding connection_binding_;
    Phase phase_ = Phase::open;

    NativeIpcWireV3::Bytes outbound_;
    std::size_t outbound_offset_ = 0;

    std::array<unsigned char,
        prefix_bytes + NativeIpcWireV3::max_receipt_frame_bytes>
        receipt_buffer_{};
    std::size_t receipt_size_ = 0;
    std::size_t receipt_target_ = 0;

    std::vector<NativeIpcReceiptTable::LostCorrelation> losses_;
};

}  // namespace oreslang::runtime
