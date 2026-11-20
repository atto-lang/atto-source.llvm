#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace oreslang {

/**
 * Runtime-only actor scalar CAS. Snapshot owns a copy, not a state loan.
 * The mutex is held only during a small snapshot/commit/retire transition;
 * never across guest code, suspension, I/O, or callbacks.
 */
class VersionedActorCell final {
    struct Core {
        explicit Core(std::int64_t initial, std::uint64_t version)
                : value(initial), version(version) {}
        std::mutex mutex;
        std::int64_t value;
        std::uint64_t version;
        bool retired = false;
    };

public:
    enum class Commit { Committed, Conflict, Foreign, Retired, Exhausted };

    class Snapshot final {
    public:
        std::int64_t value() const noexcept { return value_; }

    private:
        friend class VersionedActorCell;
        Snapshot(std::shared_ptr<Core> origin, std::int64_t value,
                 std::uint64_t version) noexcept
                : origin_(std::move(origin)), value_(value), version_(version) {}
        std::shared_ptr<Core> origin_;
        std::int64_t value_;
        std::uint64_t version_;
    };

    explicit VersionedActorCell(std::int64_t initial, std::uint64_t version = 0)
            : core_(std::make_shared<Core>(initial, version)) {
        if (version > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("actor state version out of range");
    }
    VersionedActorCell(const VersionedActorCell&) = delete;
    VersionedActorCell& operator=(const VersionedActorCell&) = delete;
    VersionedActorCell(VersionedActorCell&&) = delete;
    VersionedActorCell& operator=(VersionedActorCell&&) = delete;
    ~VersionedActorCell() noexcept { retire(); }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(core_->mutex);
        if (core_->retired) throw std::logic_error("actor state retired");
        return Snapshot(core_, core_->value, core_->version);
    }

    Commit try_commit(const Snapshot& captured, std::int64_t replacement) noexcept {
        // The retained shared_ptr makes address-reuse ABA impossible while a
        // token exists, even if a new actor incarnation takes the old address.
        if (captured.origin_.get() != core_.get()) return Commit::Foreign;
        std::lock_guard<std::mutex> lock(core_->mutex);
        if (core_->retired) return Commit::Retired;
        if (core_->version != captured.version_) return Commit::Conflict;
        if (core_->version == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return Commit::Exhausted;
        core_->value = replacement;
        ++core_->version;
        return Commit::Committed;
    }

    void retire() noexcept {
        std::lock_guard<std::mutex> lock(core_->mutex);
        core_->retired = true;
    }

private:
    std::shared_ptr<Core> core_;
};

} // namespace oreslang
