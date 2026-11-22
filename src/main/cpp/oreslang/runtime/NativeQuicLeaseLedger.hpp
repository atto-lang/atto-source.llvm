#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace oreslang::native_quic {

// Host-only native I/O retirement accounting. Reactor-confined: ONE writer.
// No heap allocation, blocking waits, or guest-callable authority in this type.
// A physical QUIC/TLS provider MUST furnish real completion receipts before
// calling settle_stream/settle_connection. Cancellation alone is not a receipt.
template <std::size_t ConnectionSlots, std::size_t StreamSlots,
          std::size_t MaxBidiPerConnection, std::size_t MaxUniPerConnection>
class NativeQuicLeaseLedger final {
    static_assert(ConnectionSlots > 0 && StreamSlots > 0, "nonzero budgets required");
    static_assert(MaxBidiPerConnection > 0 && MaxUniPerConnection > 0,
                  "nonzero per-connection stream budgets required");
public:
    struct Actor { std::uint64_t id, generation; };
    struct Connection { std::uint64_t ticket, actor_id, generation; };
    struct Stream { std::uint64_t ticket, connection_ticket, actor_id, generation; };
    enum class Direction : std::uint8_t { Bidirectional, Unidirectional };

private:
    struct ConnectionSlot {
        Connection handle{};
        std::size_t bidi = 0;
        std::size_t uni = 0;
        bool active = false;
        bool draining = false;
    };
    struct StreamSlot {
        Stream handle{};
        Direction direction = Direction::Bidirectional;
        bool active = false;
    };
    const Actor owner_;
    std::array<ConnectionSlot, ConnectionSlots> connections_{};
    std::array<StreamSlot, StreamSlots> streams_{};
    std::uint64_t next_ticket_ = 1;
    bool retiring_ = false;

    static bool same(Connection a, Connection b) noexcept {
        return a.ticket == b.ticket && a.ticket != 0 &&
            a.actor_id == b.actor_id && a.generation == b.generation;
    }
    static bool same(Stream a, Stream b) noexcept {
        return a.ticket == b.ticket && a.ticket != 0 &&
            a.connection_ticket == b.connection_ticket &&
            a.actor_id == b.actor_id && a.generation == b.generation;
    }
    ConnectionSlot* find(Connection connection) noexcept {
        for (auto& slot : connections_)
            if (slot.active && same(slot.handle, connection)) return &slot;
        return nullptr;
    }
    std::uint64_t issue_ticket() noexcept {
        if (next_ticket_ == 0) return 0; // monotonic, never wrap and reuse
        return next_ticket_++;
    }

public:
    explicit NativeQuicLeaseLedger(Actor owner) noexcept : owner_(owner) { }
    NativeQuicLeaseLedger(const NativeQuicLeaseLedger&) = delete;
    NativeQuicLeaseLedger& operator=(const NativeQuicLeaseLedger&) = delete;

    std::optional<Connection> admit_connection() noexcept {
        if (retiring_ || owner_.id == 0 || owner_.generation == 0 ||
            next_ticket_ == 0) return std::nullopt;
        for (auto& slot : connections_) {
            if (slot.active) continue;
            const auto ticket = issue_ticket();
            if (ticket == 0) return std::nullopt;
            slot = ConnectionSlot{};
            slot.handle = Connection{ticket, owner_.id, owner_.generation};
            slot.active = true;
            return slot.handle;
        }
        return std::nullopt;
    }

    std::optional<Stream> admit_stream(Connection connection,
                                       Direction direction) noexcept {
        if (direction != Direction::Bidirectional &&
            direction != Direction::Unidirectional) return std::nullopt;
        auto* conn = find(connection);
        if (retiring_ || !conn || conn->draining || next_ticket_ == 0)
            return std::nullopt;
        if (direction == Direction::Bidirectional &&
            conn->bidi >= MaxBidiPerConnection) return std::nullopt;
        if (direction == Direction::Unidirectional &&
            conn->uni >= MaxUniPerConnection) return std::nullopt;
        for (auto& slot : streams_) {
            if (slot.active) continue;
            const auto ticket = issue_ticket();
            if (ticket == 0) return std::nullopt;
            slot.handle = Stream{ticket, connection.ticket,
                                 owner_.id, owner_.generation};
            slot.direction = direction;
            slot.active = true;
            if (direction == Direction::Bidirectional) ++conn->bidi;
            else ++conn->uni;
            return slot.handle;
        }
        return std::nullopt;
    }

    bool begin_close(Connection connection) noexcept {
        auto* conn = find(connection);
        if (!conn) return false;
        conn->draining = true;
        return true;
    }
    void begin_actor_retirement() noexcept {
        retiring_ = true;
        for (auto& conn : connections_) if (conn.active) conn.draining = true;
    }

    bool settle_stream(Stream stream) noexcept {
        for (auto& slot : streams_) {
            if (!slot.active || !same(slot.handle, stream)) continue;
            auto* conn = find(Connection{stream.connection_ticket,
                                        stream.actor_id, stream.generation});
            // A stream must never settle against a missing/reused connection.
            if (!conn) return false;
            auto& count = slot.direction == Direction::Bidirectional
                ? conn->bidi : conn->uni;
            if (count == 0) return false;
            --count;
            slot = StreamSlot{};
            return true;
        }
        return false;
    }
    bool settle_connection(Connection connection) noexcept {
        auto* conn = find(connection);
        if (!conn || !conn->draining || conn->bidi != 0 || conn->uni != 0)
            return false;
        *conn = ConnectionSlot{};
        return true;
    }
    bool fully_retired() const noexcept {
        if (!retiring_) return false;
        for (const auto& conn : connections_) if (conn.active) return false;
        for (const auto& stream : streams_) if (stream.active) return false;
        return true;
    }
    std::size_t active_connections() const noexcept {
        std::size_t count = 0;
        for (const auto& conn : connections_) if (conn.active) ++count;
        return count;
    }
    std::size_t active_streams() const noexcept {
        std::size_t count = 0;
        for (const auto& stream : streams_) if (stream.active) ++count;
        return count;
    }
};

} // namespace oreslang::native_quic
