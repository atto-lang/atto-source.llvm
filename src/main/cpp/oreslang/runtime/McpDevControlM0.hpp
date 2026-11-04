#pragma once
// AttoLang M0 / LLVM-native SOURCE-FREE policy contract, not an MCP transport,
// credential provider, guest entrypoint, or production durable audit system.
// Compiled-in bounded resource descriptors require no reflection, scanning, JIT,
// guest closure or dynamic loading. All authority comes from the trusted host.
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace oreslang::runtime::mcp_m0 {

constexpr std::size_t kFrameBytes = 32768;
constexpr std::size_t kResponseBytes = 65536;
constexpr std::size_t kJsonDepth = 12;
constexpr std::size_t kPerSecond = 8;
constexpr std::size_t kPerMinute = 60;
constexpr std::size_t kInflight = 2;
constexpr std::size_t kPrincipals = 64;
constexpr std::size_t kNonces = 128;
constexpr std::uint64_t kClockSkewMs = 30000;
constexpr std::uint64_t kReplayMs = 60000;
constexpr std::uint64_t kDeadlineMs = 500;
constexpr std::string_view kVersion = "atto.mcp.dev-control.m0.v1";
constexpr std::string_view kProtocol = "2026-07-28";

enum class Scope : std::uint8_t { catalog = 1, summary = 2, metrics = 4, upgrade_status = 8 };
constexpr std::uint8_t bit(Scope scope) noexcept { return static_cast<std::uint8_t>(scope); }
constexpr std::uint64_t stable_id(std::string_view text) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (char ch : text) {
        result ^= static_cast<unsigned char>(ch);
        result *= 1099511628211ULL;
    }
    return result;
}
struct Descriptor {
    std::string_view suffix;
    Scope scope;
    std::uint64_t id;
};
constexpr std::array<Descriptor, 5> kDescriptors{{
    {"process/summary", Scope::summary, stable_id("atto.mcp.dev-control.m0.v1/process/summary")},
    {"actors/counters", Scope::metrics, stable_id("atto.mcp.dev-control.m0.v1/actors/counters")},
    {"tasks/counters", Scope::metrics, stable_id("atto.mcp.dev-control.m0.v1/tasks/counters")},
    {"scheduler/counters", Scope::metrics, stable_id("atto.mcp.dev-control.m0.v1/scheduler/counters")},
    {"upgrade/status", Scope::upgrade_status, stable_id("atto.mcp.dev-control.m0.v1/upgrade/status")},
}};
static_assert(kDescriptors.size() == 5, "M0 metadata table must be explicit");
static_assert(kDescriptors[0].id != kDescriptors[1].id, "descriptor IDs must be distinct");

// Input is a *parsed*, size/depth/duplicate-key validated host request. This
// does not parse JSON; the eventual transport must enforce those wire checks.
struct Request {
    std::string_view method, id, nonce, protocol, schema;
    std::string_view claimed_runtime, claimed_tenant, uri;
    std::uint64_t issued_at_ms = 0, expected_generation = 0;
    std::size_t frame_bytes = 0, json_depth = 0;
    // A JSON-RPC integer ID is distinct from a string with the same digits.
    // The host JSON parser must hand over the canonical decimal representation.
    bool numeric_id = false;
};
// The host must authenticate the OS peer and resolve this credential itself.
// None of these fields may be deserialized from untrusted MCP client metadata.
struct Principal {
    bool authenticated = false;
    std::string_view principal, boot, runtime, tenant;
    std::uint64_t generation = 0, expires_at_ms = 0;
    std::uint8_t scopes = 0;
};
struct Host {
    bool enabled = false, peer_verified = false;
    std::string_view boot;
    std::uint64_t generation = 0, wall_ms = 0, monotonic_ms = 0;
};
enum class Reason : std::uint8_t {
    ok, disabled, unauthenticated, audit_failure, malformed, overflow,
    version, generation, runtime, tenant, scope, unsupported,
    replay, duplicate_id, quota, timeout
};
// Auditing is a nonblocking trusted-host callback. It must return true only
// after the host has durably committed the (redacted) decision. This model
// does not implement durable storage or HMAC identity pseudonymization.
using Audit = bool (*)(Reason, bool allowed, void* user) noexcept;
struct Ticket { std::uint64_t token = 0; };
struct Result { Reason reason; Ticket ticket; bool ok() const noexcept { return reason == Reason::ok; } };

inline bool identifier(std::string_view s) noexcept {
    if (s.empty() || s.size() > 128 || s == "." || s == "..") return false;
    for (char ch : s) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) return false;
    }
    return true;
}
inline bool nonce_valid(std::string_view s) noexcept {
    if (s.size() < 32 || s.size() > 64) return false;
    for (char ch : s) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}
inline Reason classify_uri(std::string_view text, std::string_view runtime,
                           std::string_view tenant, Descriptor& output) noexcept {
    constexpr std::string_view prefix = "atto-dev://runtime/";
    constexpr std::string_view middle = "/tenant/";
    if (text.size() > 512 || text.substr(0, prefix.size()) != prefix) return Reason::malformed;
    text.remove_prefix(prefix.size());
    const auto split = text.find(middle);
    if (split == text.npos) return Reason::malformed;
    const auto runtime_id = text.substr(0, split);
    if (!identifier(runtime_id)) return Reason::malformed;
    text.remove_prefix(split + middle.size());
    const auto end = text.find('/');
    if (end == text.npos) return Reason::malformed;
    const auto tenant_id = text.substr(0, end);
    if (!identifier(tenant_id)) return Reason::malformed;
    text.remove_prefix(end + 1);
    bool known = false;
    for (auto descriptor : kDescriptors) {
        if (text == descriptor.suffix) { output = descriptor; known = true; break; }
    }
    if (!known) return Reason::malformed;
    if (runtime_id != runtime) return Reason::runtime;
    if (tenant_id != tenant) return Reason::tenant;
    return Reason::ok;
}
inline bool uri_match(std::string_view text, std::string_view runtime,
                      std::string_view tenant, Descriptor& output) noexcept {
    return classify_uri(text, runtime, tenant, output) == Reason::ok;
}

class Ledger final {
    struct Nonce { std::array<char, 64> value{}; std::uint8_t length = 0; std::uint64_t expires = 0; };
    struct Active { std::array<char, 64> id{}; std::uint8_t length = 0;
                    bool numeric_id = false;
                    std::uint64_t token = 0, deadline = 0, generation = 0, credential_expiry = 0; };
    struct State {
        bool used = false;
        std::array<char, 128> principal{}, boot{}, runtime{}, tenant{};
        std::uint8_t principal_len = 0, boot_len = 0, runtime_len = 0, tenant_len = 0;
        std::array<std::uint64_t, kPerMinute> window{};
        std::size_t window_size = 0;
        std::array<Nonce, kNonces> nonces{};
        std::array<Active, kInflight> active{};
        std::uint64_t last_tick = 0;
    };
    std::array<State, kPrincipals> states_{};
    std::mutex mutex_;
    std::uint64_t next_ticket_ = 1;
    Audit audit_ = nullptr;
    void* audit_user_ = nullptr;

    static bool equals(const std::array<char, 128>& a, std::uint8_t len,
                       std::string_view b) noexcept {
        if (len != b.size()) return false;
        for (std::size_t i = 0; i < len; ++i) if (a[i] != b[i]) return false;
        return true;
    }
    static void assign(std::array<char, 128>& out, std::uint8_t& len,
                       std::string_view text) noexcept {
        len = static_cast<std::uint8_t>(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) out[i] = text[i];
    }
    static bool matches(const State& s, const Principal& p) noexcept {
        return s.used && equals(s.principal, s.principal_len, p.principal) &&
               equals(s.boot, s.boot_len, p.boot) &&
               equals(s.runtime, s.runtime_len, p.runtime) &&
               equals(s.tenant, s.tenant_len, p.tenant);
    }
    static bool empty_active(const State& s) noexcept {
        for (const auto& a : s.active) if (a.token != 0) return false;
        return true;
    }
    static void expire(State& s, std::uint64_t now) noexcept {
        std::size_t next = 0;
        for (std::size_t i = 0; i < s.window_size; ++i) {
            if (s.window[i] <= now && now - s.window[i] < 60000) s.window[next++] = s.window[i];
        }
        s.window_size = next;
        for (auto& n : s.nonces) if (n.length && n.expires <= now) n.length = 0;
    }
    Result decision(Reason reason) noexcept {
        if (!audit_ || !audit_(reason, reason == Reason::ok, audit_user_))
            return {Reason::audit_failure, {}};
        return {reason, {}};
    }
public:
    Ledger(Audit audit, void* user) noexcept : audit_(audit), audit_user_(user) {}
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;

    Result begin(const Host& host, const Principal& p, const Request& r) noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!host.enabled) return decision(Reason::disabled);
        if (!host.peer_verified || !p.authenticated || !identifier(p.principal) ||
            !identifier(p.boot) || !identifier(p.runtime) || !identifier(p.tenant) ||
            p.generation == 0 || !p.scopes || (p.scopes & ~std::uint8_t(15)) ||
            p.expires_at_ms <= host.wall_ms) return decision(Reason::unauthenticated);
        if (p.boot != host.boot || p.generation != host.generation)
            return decision(Reason::generation);
        State* state = nullptr;
        State* unused = nullptr;
        for (auto& it : states_) {
            if (matches(it, p)) state = &it;
            if (!it.used && !unused) unused = &it;
        }
        if (!state) {
            if (!unused) {
                // Only reclaim states with expired nonce and rate history and
                // no live ticket. Failing to reclaim never grants authority.
                for (auto& it : states_) {
                    if (host.monotonic_ms < it.last_tick) continue;
                    expire(it, host.monotonic_ms);
                    bool live_nonce = false;
                    for (const auto& n : it.nonces) live_nonce |= (n.length != 0);
                    if (empty_active(it) && it.window_size == 0 && !live_nonce) {
                        it = State{};
                        unused = &it; break;
                    }
                }
            }
            if (!unused) return decision(Reason::quota);
            state = unused;
            state->used = true;
            assign(state->principal, state->principal_len, p.principal);
            assign(state->boot, state->boot_len, p.boot);
            assign(state->runtime, state->runtime_len, p.runtime);
            assign(state->tenant, state->tenant_len, p.tenant);
        }
        if (host.monotonic_ms < state->last_tick) return decision(Reason::malformed);
        expire(*state, host.monotonic_ms);
        std::size_t second = 0;
        for (std::size_t i = 0; i < state->window_size; ++i)
            if (host.monotonic_ms - state->window[i] < 1000) ++second;
        if (second >= kPerSecond || state->window_size >= kPerMinute)
            return decision(Reason::quota);
        state->window[state->window_size++] = host.monotonic_ms;
        state->last_tick = host.monotonic_ms;
        // Denied *authenticated* requests consume the rate window.
        if (r.frame_bytes == 0 || r.id.empty() || r.id.size() > 64 ||
            r.json_depth == 0) return decision(Reason::malformed);
        if (r.frame_bytes > kFrameBytes || r.json_depth > kJsonDepth)
            return decision(Reason::overflow);
        if (r.numeric_id) {
            if (r.id.size() > 16 || (r.id.size() > 1 && r.id.front() == '0'))
                return decision(Reason::malformed);
            std::uint64_t value = 0;
            for (char ch : r.id) {
                if (ch < '0' || ch > '9') return decision(Reason::malformed);
                value = value * 10 + static_cast<unsigned>(ch - '0');
            }
            if (value > 9007199254740991ULL) return decision(Reason::malformed);
        }
        if (r.protocol != kProtocol || r.schema != kVersion)
            return decision(Reason::version);
        if (r.method != "server/discover" && r.method != "tools/list" &&
            r.method != "resources/list" && r.method != "resources/read")
            return decision(Reason::unsupported);
        if (!nonce_valid(r.nonce) ||
            (r.issued_at_ms > host.wall_ms
                ? r.issued_at_ms - host.wall_ms > kClockSkewMs
                : host.wall_ms - r.issued_at_ms > kClockSkewMs))
            return decision(Reason::malformed);
        if (r.expected_generation != p.generation) return decision(Reason::generation);
        if (r.claimed_runtime != p.runtime) return decision(Reason::runtime);
        if (r.claimed_tenant != p.tenant) return decision(Reason::tenant);
        std::uint8_t scope = bit(Scope::catalog);
        if (r.method == "resources/read") {
            Descriptor descriptor{};
            const Reason uri_reason = classify_uri(r.uri, p.runtime, p.tenant, descriptor);
            if (uri_reason != Reason::ok) return decision(uri_reason);
            scope = bit(descriptor.scope);
        } else if (!r.uri.empty()) return decision(Reason::malformed);
        if (!(p.scopes & scope)) return decision(Reason::scope);
        for (const auto& n : state->nonces) {
            if (n.length == r.nonce.size()) {
                bool same = true;
                for (std::size_t i = 0; i < n.length; ++i) same &= n.value[i] == r.nonce[i];
                if (same) return decision(Reason::replay);
            }
        }
        for (const auto& a : state->active) {
            if (a.token && a.numeric_id == r.numeric_id && a.length == r.id.size()) {
                bool same = true;
                for (std::size_t i = 0; i < a.length; ++i) same &= a.id[i] == r.id[i];
                if (same) return decision(Reason::duplicate_id);
            }
        }
        Active* free_active = nullptr;
        Nonce* free_nonce = nullptr;
        for (auto& a : state->active) if (!a.token && !free_active) free_active = &a;
        for (auto& n : state->nonces) if (!n.length && !free_nonce) free_nonce = &n;
        if (!free_active || !free_nonce || next_ticket_ == 0 ||
            host.monotonic_ms > UINT64_MAX - kDeadlineMs ||
            host.monotonic_ms > UINT64_MAX - kReplayMs)
            return decision(Reason::quota);
        auto result = decision(Reason::ok);
        if (!result.ok()) return result;
        free_nonce->length = static_cast<std::uint8_t>(r.nonce.size());
        for (std::size_t i = 0; i < r.nonce.size(); ++i) free_nonce->value[i] = r.nonce[i];
        free_nonce->expires = host.monotonic_ms + kReplayMs;
        free_active->length = static_cast<std::uint8_t>(r.id.size());
        free_active->numeric_id = r.numeric_id;
        for (std::size_t i = 0; i < r.id.size(); ++i) free_active->id[i] = r.id[i];
        free_active->token = next_ticket_++;
        free_active->deadline = host.monotonic_ms + kDeadlineMs;
        free_active->generation = p.generation;
        free_active->credential_expiry = p.expires_at_ms;
        return {Reason::ok, {free_active->token}};
    }

    bool finish(Ticket ticket) noexcept {
        if (!ticket.token) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& s : states_) for (auto& a : s.active) {
            if (a.token != ticket.token) continue;
            a.token = 0;
            return true;
        }
        return false;
    }
    bool live(Ticket ticket, const Host& host) noexcept {
        if (!ticket.token) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto& s : states_) for (const auto& a : s.active) {
            if (a.token != ticket.token) continue;
            // Admission authority is bound to the authenticated host boot.
            // A generation number may be reused after restart; never allow
            // a stale ticket to survive boot rotation or peer revocation.
            return host.enabled && host.peer_verified &&
                   equals(s.boot, s.boot_len, host.boot) &&
                   host.monotonic_ms <= a.deadline &&
                   host.wall_ms < a.credential_expiry &&
                   host.generation == a.generation;
        }
        return false;
    }
};
} // namespace oreslang::runtime::mcp_m0
