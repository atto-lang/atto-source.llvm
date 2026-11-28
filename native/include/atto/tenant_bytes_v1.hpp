#pragma once
// Shared LLVM/Forge *trusted host* reference allocator, not a guest ABI.
// Arena creation must be restricted to the TenantVM runtime. This header
// cannot itself authenticate a tenant or sandbox untrusted native code.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace atto::tenant_bytes_v1 {

enum class Code { ok, invalid_owner, invalid_size, quota, allocation, bounds, value, revoked };
struct Owner {
  std::uint64_t tenant_id;
  std::uint64_t actor_id;
  std::uint64_t generation;
};
struct Ledger {
  const Owner owner;
  const std::size_t quota;
  std::size_t charged = 0;
  bool revoked = false;
  std::mutex lock;
  Ledger(Owner owner_in, std::size_t quota_in) noexcept
      : owner(owner_in), quota(quota_in) {}
};

class Arena;
class Bytes {
  std::shared_ptr<Ledger> ledger_;
  std::unique_ptr<std::uint8_t[]> storage_;
  std::size_t length_ = 0;
  std::size_t charge_ = 0;
  mutable std::mutex lock_;
  friend class Arena;
  Bytes(std::shared_ptr<Ledger> l, std::unique_ptr<std::uint8_t[]> bytes,
        std::size_t length, std::size_t charge) noexcept
      : ledger_(std::move(l)), storage_(std::move(bytes)), length_(length), charge_(charge) {}
  void release_locked() noexcept {
    if (!ledger_) return;
    // Volatile stores are a best-effort reuse scrub, not a guarantee that
    // hardware copies, paging, caches or an optimizing runtime are erased.
    volatile std::uint8_t* p = storage_.get();
    for (std::size_t i = 0; i < (length_ == 0 ? 1 : length_); ++i) p[i] = 0;
    storage_.reset();
    std::lock_guard<std::mutex> guard(ledger_->lock);
    ledger_->charged -= charge_;
    ledger_.reset();
    length_ = charge_ = 0;
  }
 public:
  Bytes(const Bytes&) = delete;
  Bytes& operator=(const Bytes&) = delete;
  Bytes(Bytes&& other) noexcept {
    std::lock_guard<std::mutex> guard(other.lock_);
    ledger_ = std::move(other.ledger_);
    storage_ = std::move(other.storage_);
    length_ = std::exchange(other.length_, 0);
    charge_ = std::exchange(other.charge_, 0);
  }
  Bytes& operator=(Bytes&& other) noexcept {
    if (this == &other) return *this;
    std::scoped_lock guards(lock_, other.lock_);
    release_locked();
    ledger_ = std::move(other.ledger_);
    storage_ = std::move(other.storage_);
    length_ = std::exchange(other.length_, 0);
    charge_ = std::exchange(other.charge_, 0);
    return *this;
  }
  ~Bytes() { std::lock_guard<std::mutex> guard(lock_); release_locked(); }
  // Trusted cancellation/actor-teardown path: release immediately, idempotently.
  // Guest code must not bypass the compiler's live-loan/drop checks.
  void drop_now() noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    release_locked();
  }
  std::size_t length() const noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    return length_;
  }
  Code read_byte(const Arena& caller, std::int64_t index, std::uint8_t& out) const noexcept;
  Code write_byte(const Arena& caller, std::int64_t index, std::int64_t value) noexcept;
};

struct Allocation {
  Code code;
  std::optional<Bytes> value;
};

class Arena {
  std::shared_ptr<Ledger> ledger_;
  friend class Bytes;
 public:
  // TRUSTED RUNTIME ONLY: guest code must not call this constructor.
  Arena(Owner principal, std::size_t max_charged_bytes)
      : ledger_(std::make_shared<Ledger>(principal, max_charged_bytes)) {}
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;
  Allocation allocate_zeroed(std::int64_t n) noexcept {
    if (!ledger_ || ledger_->owner.tenant_id == 0 ||
        ledger_->owner.actor_id == 0 || ledger_->owner.generation == 0)
      return {Code::invalid_owner, std::nullopt};
    if (n < 0) return {Code::invalid_size, std::nullopt};
    const auto length = static_cast<std::uint64_t>(n);
    // Even zero-length regions reserve one physical byte. Charge it.
    const auto capacity = length == 0 ? std::uint64_t{1} : length;
    constexpr std::size_t overhead = sizeof(Bytes);
    if (capacity > std::numeric_limits<std::size_t>::max() - overhead)
      return {Code::quota, std::nullopt};
    const auto charged = static_cast<std::size_t>(capacity) + overhead;
    {
      std::lock_guard<std::mutex> guard(ledger_->lock);
      if (ledger_->revoked) return {Code::revoked, std::nullopt};
      if (charged > ledger_->quota - ledger_->charged)
        return {Code::quota, std::nullopt};
      ledger_->charged += charged;
    }
    // new[]() zeroes all logical bytes before the region is published.
    // Allocate at least one byte for an empty region so scrubbing is safe.
    std::unique_ptr<std::uint8_t[]> bytes(
        new (std::nothrow) std::uint8_t[length ? static_cast<std::size_t>(length) : 1]());
    if (!bytes) {
      std::lock_guard<std::mutex> guard(ledger_->lock);
      ledger_->charged -= charged;
      return {Code::allocation, std::nullopt};
    }
    {
      std::lock_guard<std::mutex> guard(ledger_->lock);
      if (ledger_->revoked) {
        ledger_->charged -= charged;
        return {Code::revoked, std::nullopt};
      }
    }
    return {Code::ok, Bytes{ledger_, std::move(bytes),
                            static_cast<std::size_t>(length), charged}};
  }
  std::size_t charged() const noexcept {
    std::lock_guard<std::mutex> guard(ledger_->lock);
    return ledger_->charged;
  }
  void revoke() noexcept {
    std::lock_guard<std::mutex> guard(ledger_->lock);
    ledger_->revoked = true;
  }
};

inline Code Bytes::read_byte(const Arena& caller, std::int64_t index,
                            std::uint8_t& out) const noexcept {
  std::lock_guard<std::mutex> guard(lock_);
  if (!ledger_ || caller.ledger_.get() != ledger_.get()) return Code::invalid_owner;
  std::lock_guard<std::mutex> state_guard(ledger_->lock);
  if (ledger_->revoked) return Code::revoked;
  if (index < 0 || static_cast<std::uint64_t>(index) >= length_) return Code::bounds;
  out = storage_[static_cast<std::size_t>(index)];
  return Code::ok;
}
inline Code Bytes::write_byte(const Arena& caller, std::int64_t index,
                             std::int64_t value) noexcept {
  std::lock_guard<std::mutex> guard(lock_);
  if (!ledger_ || caller.ledger_.get() != ledger_.get()) return Code::invalid_owner;
  std::lock_guard<std::mutex> state_guard(ledger_->lock);
  if (ledger_->revoked) return Code::revoked;
  if (index < 0 || static_cast<std::uint64_t>(index) >= length_) return Code::bounds;
  if (value < 0 || value > 255) return Code::value;
  storage_[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value);
  return Code::ok;
}
} // namespace atto::tenant_bytes_v1
