#pragma once
// HOST-ONLY admission substrate. Not a guest intrinsic, durable store or signature verifier.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace oreslang::agent_dispatch {
struct Scope {
  std::string tenant_id, actor_id, exact_revision, dispatch_id, context_sha256;
  std::uint64_t generation = 0, epoch = 0, expires_at_ms = 0;
  std::uint64_t utf8_bytes = 0, measured_tokens = 0;
};
struct TrustedHost {
  std::function<bool(const Scope&)> current_revision;
  std::function<bool(const Scope&)> authorize;
  // Must be durable atomic unique (scope key, immutable context digest).
  std::function<bool(const Scope&)> claim_once;
  // Provider must independently revalidate; data-only, no tools/capabilities.
  std::function<bool(const Scope&)> submit_data_only;
  // Trusted clock, independently supplied by host and rechecked before submit.
  std::function<std::uint64_t()> now_ms;
};
inline bool safe_id(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (const unsigned char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
          c == ':' || c == '@' || c == '/' )) return false;
  return true;
}
inline bool lower_hex(const std::string& s, std::size_t length) {
  if (s.size() != length) return false;
  for (const unsigned char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
class HostGate final {
 public:
  HostGate(std::string expected_tenant, std::string expected_actor,
           std::uint64_t generation, std::uint64_t epoch,
           std::string revision, std::uint64_t token_cap)
      : tenant_(std::move(expected_tenant)), actor_(std::move(expected_actor)),
        revision_(std::move(revision)), generation_(generation), epoch_(epoch),
        token_cap_(token_cap) {}
  // The host creates Scope, including measured bytes/tokens and actual content hash.
  // Single-use even when a remote send fails ambiguously.
  bool dispatch(const Scope& scope, const TrustedHost& host, std::uint64_t now_ms) {
    if (used_.exchange(true, std::memory_order_acq_rel)) return false;
    if (!safe_id(tenant_) || !safe_id(actor_) || !safe_id(scope.dispatch_id) ||
        scope.tenant_id != tenant_ || scope.actor_id != actor_ ||
        scope.generation == 0 || scope.generation != generation_ ||
        scope.epoch == 0 || scope.epoch != epoch_ ||
        scope.exact_revision != revision_ || !lower_hex(revision_, 40) ||
        !lower_hex(scope.context_sha256, 64) ||
        scope.expires_at_ms <= now_ms || scope.utf8_bytes == 0 ||
        scope.utf8_bytes > 65536 || scope.measured_tokens == 0 ||
        scope.measured_tokens > 16384 || token_cap_ == 0 ||
        scope.measured_tokens > token_cap_ ||
        !host.current_revision || !host.authorize || !host.claim_once ||
        !host.submit_data_only || !host.now_ms) return false;
    try {
      const auto admitted_at = host.now_ms();
      if (admitted_at == 0 || scope.expires_at_ms <= admitted_at ||
          !host.current_revision(scope) || !host.authorize(scope) ||
          !host.claim_once(scope) || !host.current_revision(scope) ||
          !host.authorize(scope)) return false;
      const auto before_submit = host.now_ms();
      if (before_submit < admitted_at || scope.expires_at_ms <= before_submit)
        return false;
      return host.submit_data_only(scope);
    } catch (...) { return false; }
  }
 private:
  std::string tenant_, actor_, revision_;
  std::uint64_t generation_, epoch_, token_cap_;
  std::atomic<bool> used_{false}; // one-shot even with concurrent callers
};
} // namespace oreslang::agent_dispatch
