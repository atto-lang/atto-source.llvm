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
//  - Faults: a fault proven confined to one actor restarts that actor from its stored state, with its
//    queued mail kept. Any other fault ends that VM. The carrier thread and every other VM go on.
//  - Termination: a VM can be killed while spinning, with no cooperation from guest code,
//    by interrupting its carriers. All its memory is discarded, so nothing needs unwinding.
//  - Quotas: memory, actors, mailboxes, queued mail bytes, per-turn deadline, cumulative CPU.
//  - Fairness: carriers go to the ready VM that has used the least CPU, however its turns are sized.
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
    HostClosed,
    SignalHandlerLost, // another component replaced our fault handler, so a guest fault would be fatal
    ActorRestarted,    // this turn failed. The actor restarted from its stored state and goes on
    ActorTerminated    // the actor spent its restart budget and has ended; the rest of the VM goes on
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
    std::uint64_t max_queued_bytes = std::uint64_t{16} << 20; // host memory that mail may hold for this VM
    std::uint32_t max_parallel_turns = 4;  // carriers one tenant can hold at once
    std::uint32_t max_payload_bytes = 64 * 1024;
    std::uint32_t max_reply_bytes = 64 * 1024;
    std::uint32_t turn_deadline_ms = 1000; // CPU time of one turn, with a wall-clock backstop
    std::uint64_t cpu_budget_ms = 0;       // cumulative turn time, 0 = unlimited
    // Restarts one actor may have in the window. One more failure ends that actor for good, and
    // the rest of the VM goes on. A crash loop has to end somewhere, and stored state can be its cause.
    std::uint32_t restart_budget = 3;
    std::uint32_t restart_window_ms = 10000;
    std::uint32_t restart_backoff_ms = 10; // before the next turn, doubling with each failure in the window
};

struct VmSpec {
    std::string tenant_id; // [A-Za-z0-9_.-]{1,128}, not "." or ".."
    const OresVmGuestModule* module = nullptr; // must outlive the VM; no writable globals
    std::shared_ptr<const void> module_owner;  // released after the VM retires
    VmLimits limits;
};

struct HostLimits {
    std::uint32_t max_vms = 1000;
    std::uint32_t carriers = 8;
    std::uint64_t max_admitted_heap_bytes = std::uint64_t{1} << 40;
    std::size_t carrier_stack_bytes = std::size_t{8} << 20; // at least 512 KiB; host calls reserve 256 KiB of it
    std::uint64_t max_queued_bytes = std::uint64_t{4} << 30; // mail across all VMs
    // A fault outside any cage means the boundary itself failed: lowered code broke the
    // addressing rule, so memory safety of every tenant is in doubt. Stop rather than continue.
    bool abort_on_wild_access = false;
};

// Published only after the cage is unmapped and no carrier can be inside this VM.
struct VmTerminal {
    Phase phase = Phase::Failed;
    FailReason reason = FailReason::None;
    std::uint64_t generation = 0;
    std::uint64_t fault_offset = 0; // within the cage, when reason == MemoryFault
    // A VM can stop cleanly after some of its turns failed. These keep that from looking like a clean run.
    std::uint64_t actor_restarts = 0;
    std::uint64_t mail_lost = 0; // restarts whose failed turn had no caller to tell
    std::uint64_t actors_terminated = 0; // spent their restart budget
    std::uint64_t mail_abandoned = 0;    // queued for an actor that then ended this way
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
    std::uint64_t actor_restarts = 0;
    std::uint64_t mail_lost = 0;
    std::uint64_t actors_terminated = 0;
    std::uint64_t mail_abandoned = 0;
};

struct TurnResult {
    Status status = Status::HostClosed;
    std::int64_t guest_code = 0;
    FailReason failure = FailReason::None; // what ended the turn, when status is ActorRestarted
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
    std::uint64_t wild_accesses = 0;  // nonzero means a compiler-path bug, see HostLimits
    std::uint64_t queued_bytes = 0;
    std::uint64_t actor_restarts = 0;
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
