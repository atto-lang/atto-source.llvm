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
    // Identity remains available for later compiler-bound diagnostics. Grant
    // authority is currently the unique per-incarnation issuer_ token below.
    [[maybe_unused]] std::uint64_t tenant_;
    [[maybe_unused]] std::uint64_t actor_;
    [[maybe_unused]] std::uint64_t generation_;
    std::uint64_t span_;
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
private:
    // Caller MUST already hold gate_. No pointer or host memory is returned.
    std::optional<std::uint64_t> checked_offset_locked(
        const Grant& grant, std::uint32_t word, std::uint64_t bytes) const {
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
public:
    // Diagnostic-only: releasing the mutex before the subsequent memory
    // access allows release/retire/reuse to race it. Compiler-generated loads
    // and stores MUST NOT treat checked_offset() as an access capability.
    std::optional<std::uint64_t> checked_offset(
        const Grant& grant, std::uint32_t word, std::uint64_t bytes) const {
        std::lock_guard<std::mutex> lock(gate_);
        return checked_offset_locked(grant, word, bytes);
    }

    // Trusted HOST-only lexical access proof. Hold the same gate used for
    // release(), retire(), and issue_allocation() across the ENTIRE operation.
    // The callback must perform the memory access synchronously and must NOT
    // suspend, retain the offset for later use, or call this gate recursively.
    // A callback exception propagates but automatically releases the gate.
    // False means no user callback ran. Not a production/native guest sandbox.
    template <typename Operation>
    bool with_checked_access(const Grant& grant, std::uint32_t word,
                             std::uint64_t bytes, Operation&& operation) const {
        std::lock_guard<std::mutex> lock(gate_);
        const auto offset = checked_offset_locked(grant, word, bytes);
        if (!offset) return false;
        std::forward<Operation>(operation)(*offset);
        return true;
    }
    // Metadata-only release for existing HOST regression callers. Guest arena
    // allocators MUST NOT call this directly: it cannot guarantee that memory
    // has been scrubbed before another allocation reuses the same bytes.
    bool release(const Grant& grant) {
        std::lock_guard<std::mutex> lock(gate_);
        return !retired_ && grant.issuer_ == issuer_ &&
               live_.erase(grant.id_) == 1;
    }

    // Trusted allocator release: scrub the backing bytes WHILE holding the
    // same lock that protects checked access and allocation/reuse. If scrub
    // throws, permanently retire the incarnation and quarantine its backing
    // memory: no subsequent access or reissue is allowed before external
    // supervisor scrubbing. Never reenter or suspend inside the callback.
    template <typename Scrub>
    bool release_after_scrub(const Grant& grant, Scrub&& scrub) {
        std::lock_guard<std::mutex> lock(gate_);
        if (retired_ || grant.issuer_ != issuer_) return false;
        const auto it = live_.find(grant.id_);
        if (it == live_.end()) return false;
        try {
            std::forward<Scrub>(scrub)(it->second.start,
                                       it->second.end - it->second.start);
        } catch (...) {
            retired_ = true; // fail closed if only part of the bytes were scrubbed
            throw;
        }
        live_.erase(it);
        return true;
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
