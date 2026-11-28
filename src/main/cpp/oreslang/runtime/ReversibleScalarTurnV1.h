#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::runtime {
// Host-only primitive: caller owns the serialized actor turn and validates
// candidate invariants. No external effects, persistence, or guest ABI.
enum class TurnKind { commit, undo, redo };
enum class TurnStatus { ok, conflict, invalid_history, quota, id_reuse, invalid_command };
struct TurnReceipt { TurnStatus status; std::int64_t version, value; };

template<std::size_t History, std::size_t Commands>
class ReversibleScalarTurnV1 {
    static_assert(History > 0 && History <= 4096 && Commands > 0 && Commands <= 4096);
    struct Delta { std::int64_t id, before, after; };
    struct Entry { std::int64_t id, expected; TurnKind kind; std::int64_t argument; TurnReceipt receipt; };
    std::array<Delta, History> history_{};
    std::array<Entry, Commands> receipts_{};
    std::size_t size_ = 0, cursor_ = 0, used_ = 0;
    std::int64_t version_ = 0, value_;
    TurnReceipt fail(TurnStatus status) const noexcept { return {status, version_, value_}; }
public:
    explicit ReversibleScalarTurnV1(std::int64_t initial) noexcept : value_(initial) {}
    ReversibleScalarTurnV1(const ReversibleScalarTurnV1&) = delete;
    ReversibleScalarTurnV1& operator=(const ReversibleScalarTurnV1&) = delete;
    std::int64_t value() const noexcept { return value_; }
    std::int64_t version() const noexcept { return version_; }
    TurnReceipt apply(std::int64_t id, std::int64_t expected, TurnKind kind, std::int64_t arg) noexcept {
        if (id <= 0 || expected < 0 || (kind != TurnKind::commit && kind != TurnKind::undo && kind != TurnKind::redo))
            return fail(TurnStatus::invalid_command);
        for (std::size_t i = 0; i < used_; ++i) {
            const auto& e = receipts_[i];
            if (e.id == id) {
                if (e.expected == expected && e.kind == kind && e.argument == arg) return e.receipt;
                return fail(TurnStatus::id_reuse);
            }
        }
        if (expected != version_) return fail(TurnStatus::conflict);
        if (used_ == Commands || version_ == std::numeric_limits<std::int64_t>::max()) return fail(TurnStatus::quota);
        std::int64_t next;
        switch (kind) {
            case TurnKind::commit:
                if (cursor_ == History) return fail(TurnStatus::quota);
                next = arg;
                break;
            case TurnKind::undo:
                if (cursor_ == 0 || history_[cursor_-1].id != arg) return fail(TurnStatus::invalid_history);
                next = history_[cursor_-1].before;
                break;
            case TurnKind::redo:
                if (cursor_ == size_ || history_[cursor_].id != arg) return fail(TurnStatus::invalid_history);
                next = history_[cursor_].after;
                break;
            default: return fail(TurnStatus::invalid_command);
        }
        const TurnReceipt receipt{TurnStatus::ok, version_ + 1, next};
        if (kind == TurnKind::commit) {
            for (auto i = cursor_; i < size_; ++i) history_[i] = {};
            history_[cursor_++] = {id, value_, next};
            size_ = cursor_;
        } else if (kind == TurnKind::undo) --cursor_;
        else ++cursor_;
        receipts_[used_++] = {id, expected, kind, arg, receipt};
        value_ = next;
        ++version_;
        return receipt;
    }
};

enum class RetryBackoff { fixed, exponential };
enum class RetryDecision { wait, cancelled, deadline, ineligible, exhausted, invalid };
struct RetryOutcome { RetryDecision decision; std::int64_t delay; };
// Pure admission calculation. Recheck deadline/cancellation at actual wake.
inline RetryOutcome retry_after_failure(std::int32_t attempts, std::int32_t completed,
    std::int64_t initial, std::int64_t cap, std::int64_t deadline, std::int64_t elapsed,
    RetryBackoff backoff, bool eligible, bool cancelled) noexcept {
    using D = RetryDecision;
    if (attempts < 1 || completed < 1 || completed > attempts || initial < 0 || cap < initial ||
        deadline <= 0 || elapsed < 0 || (backoff != RetryBackoff::fixed && backoff != RetryBackoff::exponential))
        return {D::invalid, 0};
    if (cancelled) return {D::cancelled, 0};
    if (elapsed >= deadline) return {D::deadline, 0};
    if (!eligible) return {D::ineligible, 0};
    if (completed == attempts) return {D::exhausted, 0};
    auto delay = initial;
    if (backoff == RetryBackoff::exponential && delay != 0)
        for (std::int32_t i = 1; i < completed && delay < cap; ++i)
            delay = delay > cap / 2 ? cap : delay * 2;
    if (delay >= deadline - elapsed) return {D::deadline, 0};
    return {D::wait, delay};
}
} // namespace oreslang::runtime
