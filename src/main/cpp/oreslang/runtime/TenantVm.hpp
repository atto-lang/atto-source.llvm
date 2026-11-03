#pragma once
// In-process tenant VMs: one OS process hosting up to 1000 tenants, one VM per tenant,
// each with its own actors. This is the LLVM counterpart of one Graal isolate per tenant.
//
//   TenantVmHost (one per OS process)
//     carrier pool: a fixed set of OS threads shared by every VM, fair between tenants
//     +- VM "acme"  : cage (private 4 GiB window + guard), actors, generation
//     +- VM "globex": cage, actors, generation
//
// What a VM gives a tenant:
//  - Heap: a private address-space cage. Guest code addresses it by offset (see the ABI).
//  - Faults: a trap unwinds that VM's turns. The carrier thread and every other VM go on.
//  - Termination: a VM can be killed while spinning, with no cooperation from guest code,
//    by interrupting its carriers. All its memory is discarded, so nothing needs unwinding.
//  - Quotas: memory, actors, mailboxes, per-turn deadline and cumulative CPU.
//
// What it does not give: protection from code that breaks the ABI's addressing rule, or
// from a bug in this runtime. Both share one address space with every tenant, by design.
#include "oreslang/runtime/TenantVmGuestAbi.h"

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace oreslang::runtime::vm {

enum class Status {
    Ok,
    InvalidArgument,
    DuplicateTenant,
    TenantQuota,
    HeapQuota,
    OutOfAddressSpace,
    StaleHandle,
    ForeignHandle,
    NotRunning,
    Busy,
    PayloadTooLarge,
    ActorQuota,
    UnknownActor,
    ActorStopping,
    MailboxFull,
    HeapBudget,
    BadKind,
    GuestError,
    ReplyTooLarge,
    TenantFailed,
    HostClosed
};
const char* to_string(Status status) noexcept;

enum class Phase { Running, Quiescing, Stopped, Failed };

enum class FailReason {
    None,
    MemoryFault,   // access to an unmapped part of its own cage
    WildAccess,    // faulted outside its cage: the guest broke the addressing rule
    StackOverflow,
    Trap,          // illegal instruction, explicit trap, arithmetic fault
    TurnDeadline,
    CpuBudget,
    HostKill,
    HostClosed
};
const char* to_string(FailReason reason) noexcept;

struct VmLimits {
    std::uint64_t heap_bytes = std::uint64_t{64} << 20; // committed cage memory
    std::uint32_t max_actors = 4096;
    std::uint32_t mailbox_capacity = 64;   // per actor
    std::uint32_t max_inflight = 65536;    // mail queued across this VM
    std::uint32_t max_parallel_turns = 4;  // carriers one tenant can hold at once
    std::uint32_t max_payload_bytes = 64 * 1024;
    std::uint32_t max_reply_bytes = 64 * 1024;
    std::uint32_t turn_deadline_ms = 1000;
    std::uint64_t cpu_budget_ms = 0;       // cumulative turn time, 0 = unlimited
};

struct VmSpec {
    std::string tenant_id; // [A-Za-z0-9_.-]{1,128}, not "." or ".."
    const OresVmGuestModule* module = nullptr; // must outlive the VM; no writable globals
    VmLimits limits;
};

struct HostLimits {
    std::uint32_t max_vms = 1000;
    std::uint32_t carriers = 8;
    std::uint64_t max_admitted_heap_bytes = std::uint64_t{1} << 40;
    std::size_t carrier_stack_bytes = std::size_t{8} << 20;
};

// Published only after the cage is unmapped and no carrier can be inside this VM.
struct VmTerminal {
    Phase phase = Phase::Failed;
    FailReason reason = FailReason::None;
    std::uint64_t generation = 0;
    std::uint64_t fault_offset = 0; // within the cage, when reason == MemoryFault
    std::string detail;
};

struct VmSnapshot {
    Status status = Status::StaleHandle;
    Phase phase = Phase::Failed;
    std::uint64_t generation = 0;
    std::uint64_t committed_bytes = 0;
    std::uint64_t used_bytes = 0;
    std::uint64_t turns = 0;
    std::uint64_t cpu_ns = 0;
    std::size_t live_actors = 0;
    std::size_t inflight = 0;
};

struct TurnResult {
    Status status = Status::HostClosed;
    std::int64_t guest_code = 0;
    std::vector<std::uint8_t> payload;
};

class TenantVmHost;

// Opaque authority for one VM incarnation, rejected by identity if stale or foreign.
class VmHandle {
    friend class TenantVmHost;
    std::shared_ptr<const void> root_;
    std::weak_ptr<void> vm_;
    std::uint64_t generation_ = 0;
    std::shared_future<VmTerminal> closed_;
public:
    std::uint64_t generation() const noexcept { return generation_; }
};

class VmActor {
    friend class TenantVmHost;
    std::shared_ptr<const void> root_;
    std::weak_ptr<void> vm_;
    std::uint64_t generation_ = 0;
    std::uint64_t ref_ = 0;
public:
    std::uint64_t ref() const noexcept { return ref_; }
};

struct VmOpenResult { Status status = Status::HostClosed; VmHandle handle; };
struct VmSpawnResult { Status status = Status::HostClosed; VmActor actor; };

struct HostStats {
    std::size_t live_vms = 0;
    std::uint64_t admitted_heap_bytes = 0;
    std::uint64_t traps_contained = 0;
    std::uint64_t kills_delivered = 0;
};

class TenantVmHost final {
public:
    explicit TenantVmHost(HostLimits limits = {});
    // Kills every remaining VM and joins the carriers.
    ~TenantVmHost();
    TenantVmHost(const TenantVmHost&) = delete;
    TenantVmHost& operator=(const TenantVmHost&) = delete;

    VmOpenResult open(VmSpec spec);
    VmSpawnResult spawn_actor(const VmHandle& vm, std::uint32_t kind);
    // Never blocks. Exactly one terminal result is delivered per admitted call.
    std::future<TurnResult> call(const VmActor& actor, std::vector<std::uint8_t> payload);
    // Queued mail still runs; then the actor's memory is scrubbed and its slot reusable.
    Status stop_actor(const VmActor& actor);

    Status begin_shutdown(const VmHandle& vm);
    Status kill(const VmHandle& vm);
    // Safe on stale handles. Resolves only after physical retirement.
    VmTerminal wait_closed(const VmHandle& vm) const;
    VmSnapshot snapshot(const VmHandle& vm) const;
    HostStats stats() const;

private:
    struct Core;
    std::shared_ptr<Core> core_;
};

} // namespace oreslang::runtime::vm
