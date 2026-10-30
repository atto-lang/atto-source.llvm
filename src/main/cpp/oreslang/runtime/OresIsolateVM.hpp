#pragma once

#include "oreslang/runtime/OresIsolateAbi.h"
#include "oreslang/runtime/DurableLockQueues.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// TrustedInProcess is only logical separation; do not use it for hostile code.
// SandboxedProcess fails closed pending a separately supervised OS worker.
enum class IsolateMode { TrustedInProcess, SandboxedProcess };
enum class IsolatePhase { Created, Running, Quiescing, Stopped };

struct IsolateConfig {
    std::string tenant_id;
    std::uint32_t heap_limit_bytes = 0;
    std::uint32_t max_actors = 0;
    IsolateMode mode = IsolateMode::TrustedInProcess;
    // Includes queued and currently executing trusted guest invocations.
    // Bounded independently of actor admission; it is not a CPU-time limit.
    std::size_t max_inflight_guest_calls = 64;
    DurableLockQueues::Limits durable_queues{};
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
};

// One concrete tenant-owned execution domain. OresVM is the sole constructor,
// lifecycle authority and mutation interface. No guest ABI may expose this
// object or a mutable native pointer to its storage.
class IsolateVM final {
    friend class OresVM;
    explicit IsolateVM(IsolateConfig value, std::uint64_t epoch)
        : config(std::move(value)), generation(epoch), durable_queues(epoch, config.durable_queues) {}
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
    std::size_t active_guest_calls = 0;
    // Temporary serialized entry for trusted synchronous scalar guests.
    // This is not a production actor scheduler or per-actor concurrency gate.
    std::mutex guest_turn_mutex;
    std::array<std::int64_t, ORES_ISOLATE_I64_SLOTS> guest_globals{};
    DurableLockQueues durable_queues;
    std::unordered_set<std::uint64_t> active_durable_dispatches; // host-only physical pin
};

} // namespace oreslang::runtime
