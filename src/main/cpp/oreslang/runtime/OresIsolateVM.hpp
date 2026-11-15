#pragma once

#include "oreslang/runtime/OresIsolateAbi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// TrustedInProcess is only logical separation; do not use it for hostile code.
// SandboxedProcess fails closed pending a separately supervised OS worker.
enum class IsolateMode { TrustedInProcess, SandboxedProcess };
enum class IsolatePhase { Created, Running, Quiescing, Stopped };
// A resource ticket pins an actor incarnation until the native owner
// confirms physical deregistration/retirement of the underlying operation.
enum class IsolateResourceKind { Continuation, IoRegistration, Callback, MailboxLoan };
struct IsolateResourceRecord {
    std::uint64_t actor_id;
    std::uint64_t actor_generation;
    IsolateResourceKind kind;
};

struct IsolateConfig {
    std::string tenant_id;
    std::uint32_t heap_limit_bytes = 0;
    std::uint32_t max_actors = 0;
    IsolateMode mode = IsolateMode::TrustedInProcess;
    // Includes queued and currently executing trusted guest invocations.
    // Bounded independently of actor admission; it is not a CPU-time limit.
    std::size_t max_inflight_guest_calls = 64;
    // Append after existing fields to preserve positional IsolateConfig callers.
    std::uint32_t max_resource_pins = 1024;
};

// OresVM host-level reservation and queue budgets. The heap budget charges
// declared tenant heap limits, not measured resident/native process memory.
struct OresVmLimits {
    std::uint32_t max_tenants = 1024;
    std::uint64_t max_admitted_heap_bytes = std::uint64_t{1} << 32;
    std::size_t max_inflight_guest_calls = 4096;
};

struct IsolateSnapshot {
    std::string tenant_id;
    std::uint64_t generation = 0;
    IsolatePhase phase = IsolatePhase::Created;
    std::uint32_t used_bytes = 0;
    std::size_t active_actors = 0;
    std::size_t active_guest_calls = 0;
    std::size_t active_resource_pins = 0;
};

// One concrete tenant-owned execution domain. OresVM is the sole constructor,
// lifecycle authority and mutation interface. No guest ABI may expose this
// object or a mutable native pointer to its storage.
class IsolateVM final {
    friend class OresVM;
    explicit IsolateVM(IsolateConfig value, std::uint64_t epoch)
        : config(std::move(value)), generation(epoch) {}
    IsolateVM(const IsolateVM&) = delete;
    IsolateVM& operator=(const IsolateVM&) = delete;
    IsolateVM(IsolateVM&&) = delete;
    IsolateVM& operator=(IsolateVM&&) = delete;

    IsolateConfig config;
    const std::uint64_t generation;
    IsolatePhase phase = IsolatePhase::Created;
    std::vector<std::uint8_t> heap; // logical bounded tenant region
    std::unordered_map<std::uint64_t, std::uint64_t> actors;
    std::uint64_t next_actor_generation = 1;
    std::uint64_t next_resource_id = 1;
    std::unordered_map<std::uint64_t, IsolateResourceRecord> resource_pins;
    std::size_t active_guest_calls = 0;
    // Temporary serialized entry for trusted synchronous scalar guests.
    // This is not a production actor scheduler or per-actor concurrency gate.
    std::mutex guest_turn_mutex;
    std::array<std::int64_t, ORES_ISOLATE_I64_SLOTS> guest_globals{};
};

// Canonical AttoLang name for the existing, host-owned tenant execution unit.
// Historical IsolateVM remains the source/ABI spelling; neither type
// authorizes untrusted native guest code or changes physical isolation.
using TenantVM = IsolateVM;
using TenantVMConfig = IsolateConfig;
using TenantVMPhase = IsolatePhase;

} // namespace oreslang::runtime
