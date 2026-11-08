#pragma once
// Native HOST offset-grant check. NOT a CPU/OS/VM sandbox: compiler lowering
// must make this check unavoidable and keep all host base pointers inaccessible.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace atto::security {
class ActorIsolationAccessV1 final {
public:
    static constexpr std::uint64_t max_compressed32_span =
        static_cast<std::uint64_t>(UINT32_MAX) * 8ULL;
    class Grant final {
        friend class ActorIsolationAccessV1;
        std::shared_ptr<const int> issuer_;
        std::uint64_t id_;
        Grant(std::shared_ptr<const int> issuer, std::uint64_t id) noexcept
            : issuer_(std::move(issuer)), id_(id) {}
    public:
        Grant(const Grant&) = default;
        Grant& operator=(const Grant&) = default;
    };
private:
    struct Range { std::uint64_t start, end; };
    std::uint64_t tenant_, actor_, generation_, span_;
    std::shared_ptr<const int> issuer_ = std::make_shared<const int>(0);
    mutable std::mutex gate_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, Range> live_;
    bool retired_ = false;
public:
    ActorIsolationAccessV1(std::uint64_t tenant, std::uint64_t actor,
                           std::uint64_t generation, std::uint64_t span)
        : tenant_(tenant), actor_(actor), generation_(generation), span_(span) {
        if (!tenant || !actor || !generation || !span ||
            span > max_compressed32_span)
            throw std::invalid_argument("invalid actor incarnation or compressed32 span");
    }
    ActorIsolationAccessV1(const ActorIsolationAccessV1&) = delete;
    ActorIsolationAccessV1& operator=(const ActorIsolationAccessV1&) = delete;
    std::optional<Grant> issue_allocation(std::uint64_t start,
                                           std::uint64_t length) {
        std::lock_guard<std::mutex> lock(gate_);
        if (retired_ || start % 8 != 0 || !length ||
            start > span_ || length > span_ - start ||
            next_id_ == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        const auto end = start + length;
        for (const auto& item : live_)
            if (start < item.second.end && item.second.start < end)
                return std::nullopt;
        const auto id = next_id_++;
        live_.emplace(id, Range{start, end});
        return Grant(issuer_, id);
    }
    std::optional<std::uint64_t> checked_offset(
        const Grant& grant, std::uint32_t word, std::uint64_t bytes) const {
        std::lock_guard<std::mutex> lock(gate_);
        if (retired_ || grant.issuer_ != issuer_ || word == 0 || bytes == 0)
            return std::nullopt;
        const auto it = live_.find(grant.id_);
        if (it == live_.end()) return std::nullopt;
        const auto offset = (static_cast<std::uint64_t>(word) - 1ULL) * 8ULL;
        if (offset < it->second.start || offset > it->second.end ||
            bytes > it->second.end - offset)
            return std::nullopt;
        return offset; // actor-relative offset, NEVER a native address
    }
    bool release(const Grant& grant) {
        std::lock_guard<std::mutex> lock(gate_);
        // Owning allocator must scrub memory before making region reusable.
        return !retired_ && grant.issuer_ == issuer_ &&
               live_.erase(grant.id_) == 1;
    }
    void retire() {
        std::lock_guard<std::mutex> lock(gate_);
        retired_ = true;
        live_.clear();
        // Owning supervisor must first quiesce turns and FFI pins, then scrub.
    }
    bool retired() const {
        std::lock_guard<std::mutex> lock(gate_);
        return retired_;
    }
};
} // namespace atto::security
