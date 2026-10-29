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

struct IsolateConfig {
    std::string tenant_id;
    std::uint32_t heap_limit_bytes = 0;
    std::uint32_t max_actors = 0;
    IsolateMode mode = IsolateMode::TrustedInProcess;
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
    std::size_t active_guest_calls = 0;
    // Temporary serialized entry for trusted synchronous scalar guests.
    // This is not a production actor scheduler or per-actor concurrency gate.
    std::mutex guest_turn_mutex;
    std::array<std::int64_t, ORES_ISOLATE_I64_SLOTS> guest_globals{};
};

} // namespace oreslang::runtime
