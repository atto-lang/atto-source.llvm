#include "oreslang/runtime/TenantVm.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#endif

namespace oreslang::runtime::vm {

const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::InvalidArgument: return "InvalidArgument";
        case Status::DuplicateTenant: return "DuplicateTenant";
        case Status::TenantQuota: return "TenantQuota";
        case Status::HeapQuota: return "HeapQuota";
        case Status::OutOfAddressSpace: return "OutOfAddressSpace";
        case Status::StaleHandle: return "StaleHandle";
        case Status::ForeignHandle: return "ForeignHandle";
        case Status::NotRunning: return "NotRunning";
        case Status::Busy: return "Busy";
        case Status::PayloadTooLarge: return "PayloadTooLarge";
        case Status::ActorQuota: return "ActorQuota";
        case Status::UnknownActor: return "UnknownActor";
        case Status::ActorStopping: return "ActorStopping";
        case Status::MailboxFull: return "MailboxFull";
        case Status::HeapBudget: return "HeapBudget";
        case Status::BadKind: return "BadKind";
        case Status::GuestError: return "GuestError";
        case Status::ReplyTooLarge: return "ReplyTooLarge";
        case Status::TenantFailed: return "TenantFailed";
        case Status::HostClosed: return "HostClosed";
        case Status::SignalHandlerLost: return "SignalHandlerLost";
        case Status::ActorRestarted: return "ActorRestarted";
        case Status::ActorTerminated: return "ActorTerminated";
    }
    return "Unknown";
}

const char* to_string(FailReason r) noexcept {
    switch (r) {
        case FailReason::None: return "None";
        case FailReason::MemoryFault: return "MemoryFault";
        case FailReason::WildAccess: return "WildAccess";
        case FailReason::StackOverflow: return "StackOverflow";
        case FailReason::Trap: return "Trap";
        case FailReason::TurnDeadline: return "TurnDeadline";
        case FailReason::CpuBudget: return "CpuBudget";
        case FailReason::HostKill: return "HostKill";
        case FailReason::HostClosed: return "HostClosed";
    }
    return "Unknown";
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kAddressableBytes = std::uint64_t{1} << 32;
// Window, then guard for the worst legal address: a base, an offset and a displacement, each
// below 4 GiB, plus the widest access, which can straddle the end. A store that straddles a
// page boundary can write its first part before faulting, so the guard must cover all of it.
constexpr std::uint64_t kCageBytes = kAddressableBytes * 3 + (std::uint64_t{1} << 16);
constexpr std::uint64_t kCommitStepBytes = std::uint64_t{1} << 20;
constexpr std::size_t kCarrierGuardBytes = std::size_t{1} << 20;
constexpr std::size_t kSignalStackBytes = std::size_t{64} << 10;
constexpr std::size_t kHostCallStackBytes = std::size_t{256} << 10; // malloc, locks, a first dynamic link
constexpr std::uint32_t kAlignment = 16;
constexpr std::uint32_t kMaxActorBytes = std::uint32_t{1} << 30;
constexpr std::uint32_t kMaxInstanceBytes = std::uint32_t{1} << 20;
constexpr std::uint32_t kMaxGeneration = (std::uint32_t{1} << 31) - 1; // bit 63 of a ref marks guest refs
constexpr std::uint64_t kMailOverheadBytes = 256;
constexpr std::uint64_t kWallBackstopFactor = 16; // a host that is this oversubscribed is not healthy
constexpr int kHeapClasses = 21;                  // 16 B to 16 MiB; larger blocks live until the actor ends
constexpr std::uint32_t kMinHeapClassBytes = 16;
constexpr int kCarrierSpins = 200;

inline void cpu_relax() {
#if defined(__aarch64__)
    __asm__ __volatile__("isb");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause");
#endif
}
constexpr int kInterruptSignal = SIGURG;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count());
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool valid_tenant_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == "..") return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '-';
    });
}

bool valid_module(const OresVmGuestModule* m) {
    if (!m || m->abi_version != ORES_VM_GUEST_ABI_VERSION || !m->kinds || m->kind_count == 0) return false;
    for (std::uint32_t i = 0; i < m->kind_count; ++i) {
        const OresVmGuestKind& k = m->kinds[i];
        const std::uint64_t total = std::uint64_t{k.state_bytes} + k.heap_bytes + k.scratch_bytes;
        if (!k.turn || k.scratch_bytes == 0 || total > kMaxActorBytes || k.instance_bytes > kMaxInstanceBytes ||
            k.stored_bytes > kMaxInstanceBytes || (k.flags & ~std::uint32_t{ORES_VM_KIND_RESTARTABLE}) != 0) return false;
    }
    return true;
}

struct Mail {
    std::vector<std::uint8_t> bytes;
    bool has_promise = false;
    std::promise<TurnResult> promise;
};

struct Actor {
    const OresVmGuestKind* kind = nullptr;
    std::uint32_t generation = 0; // bumped at every release, so stale refs never match
    std::uint32_t region = 0;
    std::uint32_t region_bytes = 0;
    std::uint32_t heap_used = 0;
    std::unique_ptr<std::uint32_t[]> heap_free; // per size class, allocated on the first free
    std::unique_ptr<std::uint8_t[]> instance;   // outside the cage: instance_bytes, then stored_bytes
    std::deque<std::uint64_t> recent_failures_ns; // within the restart window, so at most budget + 1
    std::uint64_t backoff_until_ns = 0;           // not schedulable until the watchdog clears this
    std::uint32_t terminated_generation = std::numeric_limits<std::uint32_t>::max();
    std::deque<Mail> mailbox;
    bool live = false;
    bool running = false;
    bool queued = false;
    bool stopping = false;
};

struct Vm {
    std::mutex mutex;
    VmSpec spec;
    std::uint64_t generation = 0;
    std::uint8_t* cage = nullptr;
    std::atomic<int> phase{static_cast<int>(Phase::Running)};
    std::uint64_t committed = 0; // grows only, up to the heap limit
    std::uint64_t bump = 0;
    std::uint64_t used = 0;
    std::map<std::uint32_t, std::vector<std::uint32_t>> free_regions;
    std::vector<std::unique_ptr<Actor>> actors;
    std::vector<std::uint32_t> free_slots;
    std::size_t live_actors = 0;
    std::deque<std::uint32_t> ready;
    std::size_t inflight = 0;
    std::uint64_t queued_bytes = 0;
    std::uint64_t instance_bytes = 0;
    std::atomic<std::uint64_t> vtime_ns{0}; // the CPU this VM has been charged, for fair scheduling
    std::uint64_t actor_restarts = 0;
    std::uint64_t mail_lost = 0;
    std::uint64_t actors_terminated = 0;
    std::uint64_t mail_abandoned = 0;
    std::uint32_t running = 0;
    bool enqueued = false;
    bool retired = false;
    std::uint64_t turns = 0;
    std::atomic<std::uint64_t> cpu_ns{0};
    VmTerminal terminal;
    std::promise<VmTerminal> closed_promise;
    std::shared_future<VmTerminal> closed;

    Phase current_phase() const { return static_cast<Phase>(phase.load(std::memory_order_seq_cst)); }
};

// One per carrier thread. Everything the signal handlers touch is here, and is either
// lock-free or written only by the owning thread.
struct Carrier {
    pthread_t thread{};
    bool started = false;
    void* stack = nullptr;
    std::uint8_t* stack_guard_low = nullptr;
    void* signal_stack = nullptr;

    // Odd while inside a turn. Interrupts name the sequence they target, so a late signal
    // can never abort the carrier's next turn, which may belong to another tenant.
    std::atomic<std::uint64_t> turn_seq{0};
    std::atomic<std::uint64_t> abort_seq{0};
    std::atomic<std::uint64_t> vm_generation{0};
    std::atomic<std::uint64_t> deadline_ms{0};
    std::atomic<std::uint64_t> started_ns{0};
    std::atomic<bool> restartable_turn{false};
    // Which turn the watchdog stopped for its deadline, as opposed to its VM having failed.
    std::atomic<std::uint64_t> deadline_abort_seq{0};

    // Written only by the watchdog.
    std::uint64_t watched_seq = 0;
    std::uint64_t watched_cpu_start_ns = 0;

    volatile std::sig_atomic_t in_guest = 0;
    sigjmp_buf resume;
    FailReason trap_reason = FailReason::None;
    std::uintptr_t fault_address = 0;
    bool trap_was_checked = false; // lowered code stopped itself, as opposed to a signal stopping it
    std::uint64_t current_seq = 0; // only this thread reads this

    // Host-call context for the current turn; only this thread reads these.
    void* core = nullptr; // TenantVmHost::Core
    const std::shared_ptr<Vm>* vm = nullptr;
    Actor* actor = nullptr;
};

// Read in signal handlers, including on threads that never ran a turn. In a library that was
// dlopen'ed the default TLS model can call malloc on first touch, which a handler must not do.
#if defined(__linux__)
__attribute__((tls_model("initial-exec")))
#endif
thread_local Carrier* t_carrier = nullptr;

// On Darwin only the mask-saving form also restores the kernel's on-alt-stack state, which
// a recovery from a stack overflow depends on. Linux derives that state from the SP.
#if defined(__APPLE__)
#define ORES_VM_SETJMP(env) sigsetjmp(env, 1)
#else
#define ORES_VM_SETJMP(env) sigsetjmp(env, 0)
#endif

void unblock_recovered_signals() {
#if !defined(__APPLE__)
    sigset_t set;
    sigemptyset(&set);
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, kInterruptSignal}) sigaddset(&set, s);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
#endif
}

struct sigaction g_previous[64];

const int kFaultSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP};

void chain_previous(int signal, siginfo_t* info, void* context) {
    const struct sigaction& previous = g_previous[signal];
    if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal, info, context);
    } else if (previous.sa_handler == SIG_IGN) {
        return;
    } else if (previous.sa_handler == SIG_DFL) {
        if (signal != kInterruptSignal) ::signal(signal, SIG_DFL); // returning re-executes the fault, now fatal
    } else {
        previous.sa_handler(signal);
    }
}

// A fault while a carrier is executing guest code ends that VM. A fault anywhere else is a
// bug in the host, so it must stay fatal rather than be mistaken for a guest problem.
void on_fault(int signal, siginfo_t* info, void* context) {
    Carrier* c = t_carrier;
    if (c && c->in_guest) {
        c->in_guest = 0;
        c->fault_address = reinterpret_cast<std::uintptr_t>(info->si_addr);
        c->trap_reason = (signal == SIGSEGV || signal == SIGBUS) ? FailReason::WildAccess : FailReason::Trap;
        siglongjmp(c->resume, 1);
    }
    // Not ours. Runtimes in this process may rely on these signals (the CLR and JVM use SIGSEGV
    // for null checks), so hand over without giving up the handler for later guest faults.
    chain_previous(signal, info, context);
}

void on_interrupt(int signal, siginfo_t* info, void* context) {
    Carrier* c = t_carrier;
    if (!c) { // not a carrier: SIGURG may be meant for someone else in this process
        chain_previous(signal, info, context);
        return;
    }
    if (c->in_guest && c->abort_seq.load(std::memory_order_acquire) == c->turn_seq.load(std::memory_order_acquire)) {
        c->in_guest = 0;
        c->fault_address = 0;
        c->trap_reason = FailReason::HostKill;
        siglongjmp(c->resume, 1);
    }
}

void trap_from_guest() {
    Carrier* c = t_carrier;
    if (!c || !c->in_guest) std::abort(); // only lowered guest code may call this
    c->in_guest = 0;
    c->fault_address = 0;
    c->trap_reason = FailReason::Trap;
    c->trap_was_checked = true;
    siglongjmp(c->resume, 1);
}

void install_handlers_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction action {};
        action.sa_sigaction = on_fault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (int s : kFaultSignals)
            if (sigaction(s, &action, &g_previous[s]) != 0) std::abort();
        action.sa_sigaction = on_interrupt;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        if (sigaction(kInterruptSignal, &action, &g_previous[kInterruptSignal]) != 0) std::abort();
    });
}

bool handlers_are_ours() {
    for (int s : kFaultSignals) {
        struct sigaction current {};
        if (sigaction(s, nullptr, &current) != 0 || !(current.sa_flags & SA_SIGINFO) || current.sa_sigaction != on_fault)
            return false;
    }
    return true;
}

// CPU this thread has used. Sampled by the watchdog, so a turn pays nothing for it.
bool thread_cpu_ns(pthread_t thread, std::uint64_t& out) {
#if defined(__APPLE__)
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(thread), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS)
        return false;
    out = (std::uint64_t{static_cast<std::uint32_t>(info.user_time.seconds)} + static_cast<std::uint32_t>(info.system_time.seconds)) * 1000000000 +
          (std::uint64_t{static_cast<std::uint32_t>(info.user_time.microseconds)} + static_cast<std::uint32_t>(info.system_time.microseconds)) * 1000;
    return true;
#else
    clockid_t clock;
    struct timespec now;
    if (pthread_getcpuclockid(thread, &clock) != 0 || clock_gettime(clock, &now) != 0) return false;
    out = static_cast<std::uint64_t>(now.tv_sec) * 1000000000 + static_cast<std::uint64_t>(now.tv_nsec);
    return true;
#endif
}

// Size class of a heap block, or -1 when it is too large to be recycled.
int heap_class(std::uint32_t bytes, std::uint32_t alignment) {
    std::uint64_t size = std::max(std::max(bytes, alignment), kMinHeapClassBytes);
    int index = 0;
    for (std::uint64_t c = kMinHeapClassBytes; c < size; c <<= 1) ++index;
    return index < kHeapClasses ? index : -1;
}
std::uint64_t heap_class_bytes(int index) { return std::uint64_t{kMinHeapClassBytes} << index; }

} // namespace

struct TenantVmHost::Core {
    explicit Core(HostLimits value) : limits(value) {}

    const HostLimits limits;
    const std::shared_ptr<const void> root = std::make_shared<int>(0);

    // Order: Vm::mutex, then ready_mutex. registry_mutex is never held with either.
    mutable std::mutex registry_mutex;
    std::unordered_map<std::string, std::shared_ptr<Vm>> by_id;
    std::unordered_map<std::uint64_t, std::shared_ptr<Vm>> by_generation;
    std::uint64_t next_generation = 1;
    std::uint64_t admitted_heap = 0;

    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    // A min-heap on CPU charged, so the next carrier goes to whichever ready VM has had the least.
    // At most one entry per VM, so it stops allocating once it has grown to the busy VM count.
    struct ReadyVm {
        std::uint64_t vtime;
        std::shared_ptr<Vm> vm;
        bool operator<(const ReadyVm& other) const { return vtime > other.vtime; }
    };
    std::vector<ReadyVm> ready_vms;
    std::uint64_t min_vtime_ns = 0; // only grows, so a VM that sat idle cannot bank credit
    std::uint32_t sleeping_carriers = 0;
    std::atomic<std::size_t> ready_hint{0}; // read without the lock, only to decide whether to keep spinning
    bool stopping = false;

    std::atomic<std::uint64_t> queued_bytes{0};
    std::atomic<std::uint64_t> actor_restarts{0};

    // A min-heap on the time, of actors waiting to run again after a restart.
    struct Backoff {
        std::uint64_t until_ns;
        std::shared_ptr<Vm> vm;
        std::uint32_t slot;
        std::uint32_t generation;
        bool operator<(const Backoff& other) const { return until_ns > other.until_ns; }
    };
    std::mutex backoff_mutex;
    std::vector<Backoff> backoff;
    mutable std::atomic<std::uint64_t> wild_accesses{0}; // counted while classifying a fault

    std::vector<std::unique_ptr<Carrier>> carriers;
    std::thread watchdog;
    std::atomic<bool> watchdog_stop{false};
    std::atomic<std::uint64_t> traps_contained{0};
    std::atomic<std::uint64_t> kills_delivered{0};

    OresVmHostApi api{};

    // ------------------------------------------------------------- scheduling

    // Each ready VM holds one position however many actors it has, and positions are ordered by
    // CPU used. That is the fairness rule: a tenant with 5000 busy actors, or with turns 200
    // times longer, gets the same share of a contended carrier as one with a single quick actor.
    void schedule_locked(const std::shared_ptr<Vm>& vm) {
        if (vm->enqueued || vm->ready.empty() || vm->running >= vm->spec.limits.max_parallel_turns) return;
        const Phase phase = vm->current_phase();
        if (phase != Phase::Running && phase != Phase::Quiescing) return;
        vm->enqueued = true;
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            std::uint64_t vtime = vm->vtime_ns.load(std::memory_order_relaxed);
            if (vtime < min_vtime_ns) {
                vtime = min_vtime_ns;
                vm->vtime_ns.store(vtime, std::memory_order_relaxed);
            }
            ready_vms.push_back(ReadyVm{vtime, vm});
            std::push_heap(ready_vms.begin(), ready_vms.end());
            ready_hint.store(ready_vms.size(), std::memory_order_relaxed);
            wake = sleeping_carriers != 0;
        }
        // A wakeup is a system call. Under load the carriers are already awake, and looking.
        if (wake) ready_cv.notify_one();
    }

    void make_ready_locked(const std::shared_ptr<Vm>& vm, std::uint32_t slot) {
        Actor& actor = *vm->actors[slot];
        if (actor.queued || actor.running || actor.backoff_until_ns || actor.mailbox.empty()) return;
        actor.queued = true;
        vm->ready.push_back(slot);
        schedule_locked(vm);
    }

    void carrier_main(Carrier* c) {
        t_carrier = c;
        stack_t alt{};
        alt.ss_sp = c->signal_stack;
        alt.ss_size = kSignalStackBytes;
        if (sigaltstack(&alt, nullptr) != 0) std::abort();
        for (;;) {
            // Work usually arrives within a few turns' time, and going to sleep costs both this
            // carrier and whoever has to wake it a system call. So look for a moment first.
            for (int spin = 0; spin < kCarrierSpins && ready_hint.load(std::memory_order_relaxed) == 0; ++spin) cpu_relax();
            std::shared_ptr<Vm> vm;
            {
                std::unique_lock<std::mutex> lock(ready_mutex);
                if (!stopping && ready_vms.empty()) {
                    ++sleeping_carriers;
                    ready_cv.wait(lock, [&] { return stopping || !ready_vms.empty(); });
                    --sleeping_carriers;
                }
                if (ready_vms.empty()) return; // stopping, nothing left
                min_vtime_ns = std::max(min_vtime_ns, ready_vms.front().vtime);
                std::pop_heap(ready_vms.begin(), ready_vms.end());
                vm = std::move(ready_vms.back().vm);
                ready_vms.pop_back();
                ready_hint.store(ready_vms.size(), std::memory_order_relaxed);
            }
            run_one_turn(c, vm);
        }
    }

    void run_one_turn(Carrier* c, const std::shared_ptr<Vm>& vm) {
        Mail mail;
        Actor* actor = nullptr;
        std::uint32_t slot = 0;
        {
            std::lock_guard<std::mutex> lock(vm->mutex);
            vm->enqueued = false;
            const Phase phase = vm->current_phase();
            if ((phase != Phase::Running && phase != Phase::Quiescing) || vm->ready.empty() ||
                vm->running >= vm->spec.limits.max_parallel_turns)
                return;
            slot = vm->ready.front();
            vm->ready.pop_front();
            actor = vm->actors[slot].get();
            actor->queued = false;
            actor->running = true;
            ++vm->running;
            mail = std::move(actor->mailbox.front());
            actor->mailbox.pop_front();
            --vm->inflight;
            refund_mail_locked(*vm, mail.bytes.size());
            ++vm->turns;
            schedule_locked(vm); // others of this VM may run in parallel, at the back of the line
        }

        TurnResult result = execute(c, vm, *actor, slot, mail);

        bool trapped = false;
        bool checked = false;
        FailReason reason = FailReason::None;
        std::uint64_t fault_offset = 0;
        if (c->trap_reason != FailReason::None) {
            trapped = true;
            checked = c->trap_was_checked;
            reason = classify(*vm, c, fault_offset);
            c->trap_reason = FailReason::None;
            if (reason == FailReason::HostKill && c->deadline_abort_seq.load(std::memory_order_seq_cst) == c->current_seq)
                reason = FailReason::TurnDeadline;
            if (reason != FailReason::HostKill) traps_contained.fetch_add(1, std::memory_order_relaxed);
        }

        bool restarted = false;
        bool terminated = false;
        std::deque<Mail> abandoned;
        bool over_budget = false;
        bool escalate = trapped && reason != FailReason::HostKill;
        {
            std::lock_guard<std::mutex> lock(vm->mutex);
            const Phase phase = vm->current_phase();
            const bool alive = phase == Phase::Running || phase == Phase::Quiescing;
            if (trapped && alive && confined_to_actor(*actor, reason, checked)) {
                if (!mail.has_promise) ++vm->mail_lost;
                escalate = false;
                if (restart_allowed_locked(vm, *actor, slot)) {
                    reset_actor_locked(*vm, *actor);
                    ++vm->actor_restarts;
                    actor_restarts.fetch_add(1, std::memory_order_relaxed);
                    restarted = true;
                } else {
                    terminate_actor_locked(*vm, *actor, slot, abandoned);
                    terminated = true;
                }
            }
            if (!escalate) finish_turn_locked(vm, *actor, slot, over_budget);
        }
        if (restarted) {
            result = TurnResult{};
            result.status = Status::ActorRestarted;
            result.failure = reason;
        }
        if (terminated) {
            result = TurnResult{};
            result.status = Status::ActorTerminated;
            result.failure = reason;
            for (auto& queued : abandoned) {
                if (!queued.has_promise) continue;
                TurnResult r;
                r.status = Status::ActorTerminated;
                r.failure = reason;
                queued.promise.set_value(std::move(r));
            }
        }
        if (escalate) {
            // Fails the VM first, so that nothing else can run in this actor once it is idle.
            fail(vm, reason, fault_offset);
            std::lock_guard<std::mutex> lock(vm->mutex);
            finish_turn_locked(vm, *actor, slot, over_budget);
        }
        // After the bookkeeping, so that a caller acting on this result sees the actor idle.
        if (mail.has_promise) mail.promise.set_value(std::move(result));
        if (over_budget) fail(vm, FailReason::CpuBudget, 0);
        maybe_retire(vm);
    }

    void finish_turn_locked(const std::shared_ptr<Vm>& vm, Actor& actor, std::uint32_t slot, bool& over_budget) {
        actor.running = false;
        --vm->running;
        const std::uint64_t budget = vm->spec.limits.cpu_budget_ms;
        over_budget = budget && vm->cpu_ns.load(std::memory_order_relaxed) / 1000000 > budget;
        const Phase phase = vm->current_phase();
        if (phase == Phase::Running || phase == Phase::Quiescing) {
            if (actor.stopping && actor.mailbox.empty()) release_actor_locked(*vm, actor, slot);
            else make_ready_locked(vm, slot);
            schedule_locked(vm);
        }
    }

    // Only where the fault itself shows that memory outside this actor is untouched. A bounds-checked
    // guest stopping itself did so before the access. Running out of stack or CPU touches nothing.
    // A memory fault in such a guest should be impossible, so it says the checks failed, and is not
    // something to carry on from in this VM.
    static bool confined_to_actor(const Actor& actor, FailReason reason, bool checked) {
        if (!(actor.kind->flags & ORES_VM_KIND_RESTARTABLE)) return false;
        return (reason == FailReason::Trap && checked) || reason == FailReason::StackOverflow ||
               reason == FailReason::TurnDeadline;
    }

    // Counts this failure against the window. If it still fits, holds the actor back for a while, so
    // that a crash loop costs little until the budget ends it.
    bool restart_allowed_locked(const std::shared_ptr<Vm>& vm, Actor& actor, std::uint32_t slot) {
        const VmLimits& limits = vm->spec.limits;
        const std::uint64_t now = now_ns();
        const std::uint64_t window_ns = std::uint64_t{limits.restart_window_ms} * 1000000;
        while (!actor.recent_failures_ns.empty() && now - actor.recent_failures_ns.front() >= window_ns)
            actor.recent_failures_ns.pop_front();
        actor.recent_failures_ns.push_back(now);
        const std::size_t failures = actor.recent_failures_ns.size();
        if (failures > limits.restart_budget) return false;
        if (limits.restart_backoff_ms) {
            const std::uint64_t delay_ns = (std::uint64_t{limits.restart_backoff_ms} * 1000000) << std::min<std::size_t>(failures - 1, 16);
            actor.backoff_until_ns = now + delay_ns;
            std::lock_guard<std::mutex> lock(backoff_mutex);
            backoff.push_back(Backoff{actor.backoff_until_ns, vm, slot, actor.generation});
            std::push_heap(backoff.begin(), backoff.end());
        }
        return true;
    }

    // The watchdog's job, since nothing else runs while every actor of a VM may be waiting.
    void release_backoffs(std::uint64_t now) {
        std::vector<Backoff> due;
        {
            std::lock_guard<std::mutex> lock(backoff_mutex);
            while (!backoff.empty() && backoff.front().until_ns <= now) {
                std::pop_heap(backoff.begin(), backoff.end());
                due.push_back(std::move(backoff.back()));
                backoff.pop_back();
            }
        }
        for (const Backoff& entry : due) {
            std::lock_guard<std::mutex> lock(entry.vm->mutex);
            Actor& actor = *entry.vm->actors[entry.slot];
            if (!actor.live || actor.generation != entry.generation || actor.backoff_until_ns != entry.until_ns) continue;
            actor.backoff_until_ns = 0;
            const Phase phase = entry.vm->current_phase();
            if (phase == Phase::Running || phase == Phase::Quiescing) make_ready_locked(entry.vm, entry.slot);
        }
    }

    // Its queued mail has nowhere to go, so it is reported rather than dropped. The caller of each
    // queued call hears, and the VM counts the rest, so that this cannot look like a clean finish.
    void terminate_actor_locked(Vm& vm, Actor& actor, std::uint32_t slot, std::deque<Mail>& abandoned) {
        for (auto& mail : actor.mailbox) {
            refund_mail_locked(vm, mail.bytes.size());
            --vm.inflight;
            ++vm.mail_abandoned;
        }
        abandoned = std::move(actor.mailbox);
        actor.mailbox.clear();
        ++vm.actors_terminated;
        actor.terminated_generation = actor.generation;
        release_actor_locked(vm, actor, slot);
    }

    bool was_terminated_locked(Vm& vm, std::uint64_t ref) const {
        const auto slot = static_cast<std::uint32_t>(ref);
        if (slot >= vm.actors.size()) return false;
        const auto generation = static_cast<std::uint32_t>((ref & ~(std::uint64_t{1} << 63)) >> 32);
        return vm.actors[slot]->terminated_generation == generation;
    }

    // As it was when spawned, except for what the guest committed to its stored state, and its mail.
    void reset_actor_locked(Vm& vm, Actor& actor) {
        std::memset(vm.cage + actor.region, 0, actor.region_bytes);
        if (actor.kind->instance_bytes) std::memset(actor.instance.get(), 0, actor.kind->instance_bytes);
        actor.heap_used = 0;
        actor.heap_free.reset();
    }

    FailReason classify(const Vm& vm, Carrier* c, std::uint64_t& fault_offset) const {
        if (c->trap_reason != FailReason::WildAccess) return c->trap_reason;
        const auto address = c->fault_address;
        const auto cage = reinterpret_cast<std::uintptr_t>(vm.cage);
        if (address >= cage && address - cage < kCageBytes) {
            fault_offset = address - cage;
            return FailReason::MemoryFault;
        }
        const auto guard = reinterpret_cast<std::uintptr_t>(c->stack_guard_low);
        if (address >= guard && address - guard < kCarrierGuardBytes) return FailReason::StackOverflow;
        wild_accesses.fetch_add(1, std::memory_order_relaxed);
        if (limits.abort_on_wild_access) std::abort();
        return FailReason::WildAccess;
    }

    // Trivially destructible locals only: a recovery longjmps over this frame's callee.
    static bool enter_guest(Carrier* c, const OresVmGuestKind* kind, const OresVmHostApi* api, void* instance,
                            std::uint8_t* cage, std::uint64_t self, std::uint32_t state, std::uint32_t input,
                            std::uint32_t input_length, std::uint32_t output, std::uint32_t capacity,
                            std::int64_t* out) {
        if (ORES_VM_SETJMP(c->resume) != 0) {
            c->in_guest = 0;
            unblock_recovered_signals();
            return false;
        }
        c->in_guest = 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        // An interrupt that landed before in_guest was set must not be lost.
        if (c->abort_seq.load(std::memory_order_acquire) == c->turn_seq.load(std::memory_order_acquire)) {
            c->in_guest = 0;
            c->trap_reason = FailReason::HostKill;
            return false;
        }
        const std::int64_t value = kind->turn(api, instance, cage, self, state, input, input_length, output, capacity);
        c->in_guest = 0;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        *out = value;
        return true;
    }

    TurnResult execute(Carrier* c, const std::shared_ptr<Vm>& shared, Actor& actor, std::uint32_t slot, const Mail& mail) {
        Vm& vm = *shared;
        TurnResult result;
        const OresVmGuestKind* kind = actor.kind;
        const std::uint32_t scratch = actor.region + kind->state_bytes + kind->heap_bytes;
        const auto input_length = static_cast<std::uint32_t>(mail.bytes.size());
        const auto output = static_cast<std::uint32_t>(align_up(scratch + std::uint64_t{input_length}, kAlignment));
        const std::uint32_t scratch_end = scratch + kind->scratch_bytes;
        // Alignment can carry the output past the end of a small scratch area. The host reads the
        // reply from here afterward, so capacity must never reach beyond this actor's region.
        const std::uint32_t capacity = output < scratch_end ? std::min(scratch_end - output, vm.spec.limits.max_reply_bytes) : 0;
        if (input_length) std::memcpy(vm.cage + scratch, mail.bytes.data(), input_length);
        const std::uint64_t self = (std::uint64_t{actor.generation} << 32) | slot;

        c->vm = &shared;
        c->actor = &actor;
        c->trap_reason = FailReason::None;
        c->trap_was_checked = false;
        const std::uint64_t started = now_ns();
        c->started_ns.store(started, std::memory_order_relaxed);
        c->deadline_ms.store(vm.spec.limits.turn_deadline_ms, std::memory_order_relaxed);
        c->restartable_turn.store((kind->flags & ORES_VM_KIND_RESTARTABLE) != 0, std::memory_order_relaxed);
        c->vm_generation.store(vm.generation, std::memory_order_seq_cst);
        c->current_seq = c->turn_seq.fetch_add(1, std::memory_order_seq_cst) + 1; // now odd
        std::int64_t value = 0;
        bool completed = false;
        // Pairs with fail(): it publishes the phase and then scans carriers, this announces
        // the turn and then reads the phase, so at least one of the two sees the other.
        if (vm.current_phase() == Phase::Running || vm.current_phase() == Phase::Quiescing) {
            completed = enter_guest(c, kind, &api, actor.instance.get(), vm.cage, self, actor.region, scratch,
                                    input_length, output, capacity, &value);
        } else {
            c->trap_reason = FailReason::HostKill;
        }
        c->turn_seq.fetch_add(1, std::memory_order_seq_cst); // even again
        const std::uint64_t spent = now_ns() - started;
        vm.cpu_ns.fetch_add(spent, std::memory_order_relaxed);
        vm.vtime_ns.fetch_add(spent, std::memory_order_relaxed);
        c->vm = nullptr;
        c->actor = nullptr;

        if (!completed) {
            result.status = Status::TenantFailed;
            return result;
        }
        if (value < 0) {
            result.status = Status::GuestError;
            result.guest_code = value;
            return result;
        }
        if (static_cast<std::uint64_t>(value) > capacity) {
            result.status = Status::ReplyTooLarge;
            return result;
        }
        result.status = Status::Ok;
        result.payload.assign(vm.cage + output, vm.cage + output + value);
        return result;
    }

    // ------------------------------------------------------------- mail quota

    // Mail is host memory that tenants choose the size of, so it is charged twice: to the VM, so
    // no tenant can hold more than its share, and to the host, so all of them together cannot
    // run the process out of memory. That would end every tenant, not just the greedy one.
    bool charge_mail_locked(Vm& vm, std::size_t bytes) {
        const std::uint64_t cost = bytes + kMailOverheadBytes;
        if (cost > vm.spec.limits.max_queued_bytes - std::min(vm.spec.limits.max_queued_bytes, vm.queued_bytes)) return false;
        std::uint64_t total = queued_bytes.load(std::memory_order_relaxed);
        do {
            if (cost > limits.max_queued_bytes - std::min(limits.max_queued_bytes, total)) return false;
        } while (!queued_bytes.compare_exchange_weak(total, total + cost, std::memory_order_relaxed));
        vm.queued_bytes += cost;
        return true;
    }
    void refund_mail_locked(Vm& vm, std::size_t bytes) {
        const std::uint64_t cost = bytes + kMailOverheadBytes;
        vm.queued_bytes -= cost;
        queued_bytes.fetch_sub(cost, std::memory_order_relaxed);
    }

    // ----------------------------------------------------------- guest memory

    bool ensure_committed_locked(Vm& vm, std::uint64_t end) {
        if (end <= vm.committed) return true;
        if (end > vm.spec.limits.heap_bytes + page_size()) return false;
        const std::uint64_t target = std::min(align_up(std::max(end, vm.committed + kCommitStepBytes), page_size()),
                                              align_up(vm.spec.limits.heap_bytes + page_size(), page_size()));
        if (mprotect(vm.cage + vm.committed, target - vm.committed, PROT_READ | PROT_WRITE) != 0) return false;
        vm.committed = target;
        return true;
    }

    static std::uint64_t page_size() {
        static const std::uint64_t size = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
        return size;
    }

    // Offset 0 stays unmapped, so a null offset faults like a null pointer.
    bool allocate_region_locked(Vm& vm, std::uint32_t bytes, std::uint32_t& out) {
        auto reuse = vm.free_regions.find(bytes);
        if (reuse != vm.free_regions.end() && !reuse->second.empty()) {
            out = reuse->second.back();
            reuse->second.pop_back();
            vm.used += bytes;
            return true;
        }
        const std::uint64_t start = vm.bump;
        const std::uint64_t end = start + bytes;
        if (end - page_size() > vm.spec.limits.heap_bytes || !ensure_committed_locked(vm, end)) return false;
        vm.bump = end;
        vm.used += bytes;
        out = static_cast<std::uint32_t>(start);
        return true;
    }

    Status spawn_locked(const std::shared_ptr<Vm>& vm, std::uint32_t kind_index, std::uint64_t& out_ref) {
        Vm& v = *vm;
        if (kind_index >= v.spec.module->kind_count) return Status::BadKind;
        const OresVmGuestKind* kind = &v.spec.module->kinds[kind_index];
        if (v.live_actors >= v.spec.limits.max_actors) return Status::ActorQuota;
        const auto bytes = static_cast<std::uint32_t>(align_up(
            std::uint64_t{kind->state_bytes} + kind->heap_bytes + kind->scratch_bytes, kAlignment));
        if (std::uint64_t{kind->instance_bytes} + kind->stored_bytes > v.spec.limits.heap_bytes - std::min(v.spec.limits.heap_bytes, v.instance_bytes))
            return Status::HeapBudget;
        std::unique_ptr<std::uint8_t[]> instance;
        const std::uint64_t instance_total = std::uint64_t{kind->instance_bytes} + kind->stored_bytes;
        if (instance_total) instance.reset(new std::uint8_t[instance_total]());
        std::uint32_t region = 0;
        if (!allocate_region_locked(v, bytes, region)) return Status::HeapBudget;
        std::uint32_t slot;
        if (!v.free_slots.empty()) {
            slot = v.free_slots.back();
            v.free_slots.pop_back();
        } else {
            slot = static_cast<std::uint32_t>(v.actors.size());
            v.actors.push_back(std::make_unique<Actor>());
        }
        Actor& actor = *v.actors[slot];
        actor.kind = kind;
        actor.region = region;
        actor.region_bytes = bytes;
        actor.heap_used = 0;
        actor.heap_free.reset();
        actor.instance = std::move(instance);
        v.instance_bytes += std::uint64_t{kind->instance_bytes} + kind->stored_bytes;
        actor.recent_failures_ns.clear();
        actor.backoff_until_ns = 0;
        actor.live = true;
        actor.stopping = false;
        ++v.live_actors;
        out_ref = (std::uint64_t{actor.generation} << 32) | slot;
        return Status::Ok;
    }

    // Scrubs before reuse, so a later actor can never read what this one left behind.
    void release_actor_locked(Vm& vm, Actor& actor, std::uint32_t slot) {
        std::memset(vm.cage + actor.region, 0, actor.region_bytes);
        vm.free_regions[actor.region_bytes].push_back(actor.region);
        vm.used -= actor.region_bytes;
        vm.instance_bytes -= std::uint64_t{actor.kind->instance_bytes} + actor.kind->stored_bytes;
        actor.instance.reset();
        actor.heap_free.reset();
        actor.live = false;
        actor.stopping = false;
        ++actor.generation;
        // Past the last generation a ref could be reused, so this slot is simply retired.
        if (actor.generation < kMaxGeneration) vm.free_slots.push_back(slot);
        --vm.live_actors;
    }

    Actor* resolve_actor_locked(Vm& vm, std::uint64_t ref) {
        const auto slot = static_cast<std::uint32_t>(ref);
        if (slot >= vm.actors.size()) return nullptr;
        Actor* actor = vm.actors[slot].get();
        const auto generation = static_cast<std::uint32_t>((ref & ~(std::uint64_t{1} << 63)) >> 32);
        if (!actor->live || actor->generation != generation) return nullptr;
        return actor;
    }

    // --------------------------------------------------------------- host API

    // Guest code runs with in_guest set. Host code must clear it, so that a fault in the host
    // is treated as the host bug it is, and so that an interrupt defers to the way back.
    //
    // Host calls run on the guest's stack, and a guest chooses how deep that is. So running out
    // of stack here is the guest's doing, and must not reach host code, where it would be fatal
    // for every tenant. in_guest is still set, so if these frames touch the guard that is
    // contained as well.
    static Carrier* host_enter() {
        Carrier* c = t_carrier;
        const auto sp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
        const auto stack_low = reinterpret_cast<std::uintptr_t>(c->stack_guard_low) + kCarrierGuardBytes;
        if (sp < stack_low + kHostCallStackBytes) {
            c->in_guest = 0;
            c->fault_address = 0;
            c->trap_reason = FailReason::StackOverflow;
            siglongjmp(c->resume, 1);
        }
        c->in_guest = 0;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return c;
    }
    static void host_leave(Carrier* c) {
        if (c->abort_seq.load(std::memory_order_acquire) == c->turn_seq.load(std::memory_order_acquire)) {
            c->trap_reason = FailReason::HostKill;
            siglongjmp(c->resume, 1);
        }
        c->in_guest = 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    // Only this actor's turn touches its heap, and at most one turn runs per actor. Free lists
    // are threaded through the blocks themselves, so they cost the host no memory whatever a
    // guest does. A guest can overwrite those links, so every link is checked when followed:
    // the worst it can arrange is for its own heap to hand out overlapping blocks of itself.
    static std::uint32_t alloc_impl(Vm& vm, Actor& actor, std::uint32_t bytes, std::uint32_t alignment) {
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > 4096) return 0;
        const std::uint64_t heap = actor.region + std::uint64_t{actor.kind->state_bytes};
        const std::uint64_t heap_end = heap + actor.kind->heap_bytes;
        const int index = heap_class(bytes, alignment);
        const std::uint64_t size = index >= 0 ? heap_class_bytes(index) : bytes;
        const std::uint64_t block_alignment = index >= 0 ? std::min<std::uint64_t>(size, 4096) : alignment;
        if (index >= 0 && actor.heap_free && actor.heap_free[index] != 0) {
            const std::uint32_t block = actor.heap_free[index];
            std::uint32_t next = 0;
            std::memcpy(&next, vm.cage + block, sizeof next); // once: the value may change under us
            const bool valid = next != 0 && next >= heap && next + size <= heap_end && next % block_alignment == 0;
            actor.heap_free[index] = valid ? next : 0;
            std::memset(vm.cage + block, 0, sizeof next);
            return block;
        }
        const std::uint64_t start = align_up(heap + actor.heap_used, block_alignment);
        const std::uint64_t end = start + size;
        if (end > heap_end) return 0;
        actor.heap_used = static_cast<std::uint32_t>(end - heap);
        return static_cast<std::uint32_t>(start);
    }

    static int free_impl(Vm& vm, Actor& actor, std::uint32_t offset, std::uint32_t bytes, std::uint32_t alignment) {
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > 4096) return 0;
        const int index = heap_class(bytes, alignment);
        if (index < 0) return 0; // too large to recycle; it lasts until the actor ends
        const std::uint64_t size = heap_class_bytes(index);
        const std::uint64_t heap = actor.region + std::uint64_t{actor.kind->state_bytes};
        if (offset < heap || offset + size > heap + actor.heap_used || offset % std::min<std::uint64_t>(size, 4096) != 0) return 0;
        if (!actor.heap_free) {
            actor.heap_free.reset(new std::uint32_t[kHeapClasses]());
        }
        std::memcpy(vm.cage + offset, &actor.heap_free[index], sizeof(std::uint32_t));
        actor.heap_free[index] = offset;
        return 1;
    }

    // Host calls run on the guest's stack, under a C frame. No exception may cross it.
    static std::uint32_t host_alloc(std::uint32_t bytes, std::uint32_t alignment) {
        Carrier* c = host_enter();
        const std::uint32_t result = alloc_impl(**c->vm, *c->actor, bytes, alignment);
        host_leave(c);
        return result;
    }

    static int host_free(std::uint32_t offset, std::uint32_t bytes, std::uint32_t alignment) {
        Carrier* c = host_enter();
        int result = 0;
        try {
            result = free_impl(**c->vm, *c->actor, offset, bytes, alignment);
        } catch (...) {
        }
        host_leave(c);
        return result;
    }

    int send_impl(const std::shared_ptr<Vm>& shared, std::uint64_t target, std::uint32_t offset, std::uint32_t length) {
        Vm& vm = *shared;
        if (length > vm.spec.limits.max_payload_bytes) return 0;
        std::lock_guard<std::mutex> lock(vm.mutex);
        if (vm.current_phase() != Phase::Running) return 0;
        // The host is about to read these bytes with faults fatal, so the range must be mapped:
        // above the unmapped first page, and within what is committed, which never shrinks here.
        if (length != 0 && (offset < page_size() || std::uint64_t{offset} + length > vm.committed)) return 0;
        Actor* actor = resolve_actor_locked(vm, target);
        if (!actor || actor->stopping || actor->mailbox.size() >= vm.spec.limits.mailbox_capacity ||
            vm.inflight >= vm.spec.limits.max_inflight ||
            length > mail_capacity(*actor->kind, vm.spec.limits))
            return 0;
        Mail mail;
        mail.bytes.assign(vm.cage + offset, vm.cage + offset + length);
        if (!charge_mail_locked(vm, length)) return 0;
        actor->mailbox.push_back(std::move(mail));
        ++vm.inflight;
        make_ready_locked(shared, static_cast<std::uint32_t>(target));
        return 1;
    }

    static int host_send(std::uint64_t target, std::uint32_t offset, std::uint32_t length) {
        Carrier* c = host_enter();
        int result = 0;
        try {
            result = static_cast<Core*>(c->core)->send_impl(*c->vm, target, offset, length);
        } catch (...) { // out of memory is this guest's refusal, not the process's end
        }
        host_leave(c);
        return result;
    }

    std::uint64_t spawn_impl(const std::shared_ptr<Vm>& shared, std::uint32_t kind) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->current_phase() != Phase::Running) return 0;
        std::uint64_t ref = 0;
        if (spawn_locked(shared, kind, ref) != Status::Ok) return 0;
        return ref | (std::uint64_t{1} << 63); // so slot 0, generation 0 is not the failure value
    }

    static std::uint64_t host_spawn(std::uint32_t kind) {
        Carrier* c = host_enter();
        std::uint64_t result = 0;
        try {
            result = static_cast<Core*>(c->core)->spawn_impl(*c->vm, kind);
        } catch (...) {
        }
        host_leave(c);
        return result;
    }

    int stop_impl(const std::shared_ptr<Vm>& shared, std::uint64_t target) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (shared->current_phase() != Phase::Running) return 0;
        Actor* actor = resolve_actor_locked(*shared, target);
        if (!actor || actor->stopping) return 0;
        actor->stopping = true;
        if (!actor->running && actor->mailbox.empty())
            release_actor_locked(*shared, *actor, static_cast<std::uint32_t>(target));
        return 1;
    }

    static int host_stop(std::uint64_t target) {
        Carrier* c = host_enter();
        const int result = static_cast<Core*>(c->core)->stop_impl(*c->vm, target);
        host_leave(c);
        return result;
    }

    static std::uint32_t mail_capacity(const OresVmGuestKind& kind, const VmLimits& limits) {
        // Input shares scratch with output. Half each keeps a reply possible.
        return std::min(kind.scratch_bytes / 2, limits.max_payload_bytes);
    }

    // -------------------------------------------------------------- lifecycle

    // Stop the VM's carriers wherever they are. Signals carry the targeted sequence, so one
    // arriving late is ignored, and the watchdog repeats them until the turn has ended.
    void interrupt_carriers(std::uint64_t generation) {
        for (auto& owned : carriers) {
            Carrier& c = *owned;
            const std::uint64_t seq = c.turn_seq.load(std::memory_order_seq_cst);
            if (!(seq & 1) || c.vm_generation.load(std::memory_order_seq_cst) != generation) continue;
            if (c.turn_seq.load(std::memory_order_seq_cst) != seq) continue;
            c.abort_seq.store(seq, std::memory_order_seq_cst);
            pthread_kill(c.thread, kInterruptSignal);
            kills_delivered.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void fail(const std::shared_ptr<Vm>& vm, FailReason reason, std::uint64_t fault_offset) {
        std::deque<Mail> doomed;
        {
            std::lock_guard<std::mutex> lock(vm->mutex);
            const Phase phase = vm->current_phase();
            if (phase == Phase::Failed || phase == Phase::Stopped) return;
            vm->phase.store(static_cast<int>(Phase::Failed), std::memory_order_seq_cst);
            vm->terminal.reason = reason;
            vm->terminal.fault_offset = fault_offset;
            for (auto& actor : vm->actors) {
                for (auto& mail : actor->mailbox) {
                    refund_mail_locked(*vm, mail.bytes.size());
                    doomed.push_back(std::move(mail));
                }
                actor->mailbox.clear();
                actor->queued = false;
            }
            vm->ready.clear();
            vm->inflight = 0;
        }
        for (auto& mail : doomed) {
            if (!mail.has_promise) continue;
            TurnResult r;
            r.status = Status::TenantFailed;
            mail.promise.set_value(std::move(r));
        }
        interrupt_carriers(vm->generation);
        maybe_retire(vm);
    }

    // The cage goes away only when no carrier can still be inside it.
    void maybe_retire(const std::shared_ptr<Vm>& vm) {
        {
            std::lock_guard<std::mutex> lock(vm->mutex);
            if (vm->retired || vm->running != 0) return;
            const Phase phase = vm->current_phase();
            if (phase == Phase::Quiescing) {
                if (vm->inflight != 0) return;
                vm->phase.store(static_cast<int>(Phase::Stopped), std::memory_order_seq_cst);
            } else if (phase != Phase::Failed) {
                return;
            }
            vm->retired = true;
        }
        munmap(vm->cage, kCageBytes);
        vm->cage = nullptr;
        VmTerminal terminal = vm->terminal;
        terminal.phase = vm->current_phase();
        terminal.generation = vm->generation;
        {
            std::lock_guard<std::mutex> lock(vm->mutex);
            terminal.actor_restarts = vm->actor_restarts;
            terminal.mail_lost = vm->mail_lost;
            terminal.actors_terminated = vm->actors_terminated;
            terminal.mail_abandoned = vm->mail_abandoned;
        }
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            admitted_heap -= vm->spec.limits.heap_bytes;
            by_generation.erase(vm->generation);
            auto it = by_id.find(vm->spec.tenant_id);
            if (it != by_id.end() && it->second == vm) by_id.erase(it);
        }
        vm->closed_promise.set_value(terminal);
    }

    void watchdog_main() {
        while (!watchdog_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const std::uint64_t now = now_ns();
            release_backoffs(now);
            for (auto& owned : carriers) {
                Carrier& c = *owned;
                const std::uint64_t seq = c.turn_seq.load(std::memory_order_seq_cst);
                if (!(seq & 1)) continue;
                if (c.abort_seq.load(std::memory_order_seq_cst) == seq) {
                    // Already targeted and still inside: it may have been in a host call.
                    pthread_kill(c.thread, kInterruptSignal);
                    continue;
                }
                // A deadline counts the CPU this turn has used. On a host short of CPU, whether
                // from another tenant or anything else, a turn that is merely waiting for a core
                // has done nothing wrong. The wall clock remains as a backstop, far out.
                std::uint64_t cpu = 0;
                const bool have_cpu = thread_cpu_ns(c.thread, cpu);
                if (c.watched_seq != seq) {
                    c.watched_seq = seq;
                    c.watched_cpu_start_ns = cpu;
                }
                const std::uint64_t deadline_ns = c.deadline_ms.load(std::memory_order_relaxed) * 1000000;
                const std::uint64_t started = c.started_ns.load(std::memory_order_relaxed);
                const std::uint64_t wall = now > started ? now - started : 0;
                if (wall < deadline_ns) continue;
                const bool cpu_spent = have_cpu && cpu - c.watched_cpu_start_ns >= deadline_ns;
                if (have_cpu && !cpu_spent && wall < deadline_ns * kWallBackstopFactor) continue;
                const std::uint64_t generation = c.vm_generation.load(std::memory_order_seq_cst);
                if (c.turn_seq.load(std::memory_order_seq_cst) != seq) continue;
                if (c.restartable_turn.load(std::memory_order_relaxed)) {
                    // Stop this turn alone. If it was the last restart allowed, the carrier fails the
                    // VM on the way out. The sequence means a late signal cannot hit the next turn.
                    c.deadline_abort_seq.store(seq, std::memory_order_seq_cst);
                    c.abort_seq.store(seq, std::memory_order_seq_cst);
                    pthread_kill(c.thread, kInterruptSignal);
                    kills_delivered.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                std::shared_ptr<Vm> vm;
                {
                    std::lock_guard<std::mutex> lock(registry_mutex);
                    auto it = by_generation.find(generation);
                    if (it != by_generation.end()) vm = it->second;
                }
                if (vm) fail(vm, FailReason::TurnDeadline, 0);
            }
        }
    }

    void stop_carriers() {
        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            stopping = true;
        }
        ready_cv.notify_all();
        for (auto& carrier : carriers) {
            if (carrier->started) pthread_join(carrier->thread, nullptr);
            if (carrier->stack) munmap(carrier->stack, kCarrierGuardBytes + limits.carrier_stack_bytes);
            if (carrier->signal_stack) munmap(carrier->signal_stack, kSignalStackBytes);
        }
    }

    std::shared_ptr<Vm> resolve(const std::shared_ptr<const void>& handle_root, const std::weak_ptr<void>& weak,
                                std::uint64_t generation, Status& status) const {
        if (handle_root != root) {
            status = Status::ForeignHandle;
            return nullptr;
        }
        auto locked = std::static_pointer_cast<Vm>(weak.lock());
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = by_generation.find(generation);
        if (!locked || it == by_generation.end() || it->second != locked) {
            status = Status::StaleHandle;
            return nullptr;
        }
        status = Status::Ok;
        return locked;
    }
};

namespace {
std::future<TurnResult> ready_result(Status status) {
    std::promise<TurnResult> promise;
    TurnResult result;
    result.status = status;
    promise.set_value(std::move(result));
    return promise.get_future();
}
} // namespace

TenantVmHost::TenantVmHost(HostLimits limits) : core_(std::make_shared<Core>(limits)) {
    if (limits.carriers == 0 || limits.max_vms == 0 || limits.carrier_stack_bytes < 2 * kHostCallStackBytes)
        throw std::invalid_argument("TenantVmHost limits");
    install_handlers_once();
    Core* core = core_.get();
    core->api.abi_version = ORES_VM_GUEST_ABI_VERSION;
    core->api.alloc = &Core::host_alloc;
    core->api.free = &Core::host_free;
    core->api.send = &Core::host_send;
    core->api.spawn = &Core::host_spawn;
    core->api.stop = &Core::host_stop;
    core->api.trap = &trap_from_guest;

    const auto give_up = [core](const char* what) {
        core->stop_carriers(); // nothing else can be running yet, and Core must not outlive a carrier
        throw std::runtime_error(what);
    };
    for (std::uint32_t i = 0; i < limits.carriers; ++i) {
        auto carrier = std::make_unique<Carrier>();
        carrier->core = core;
        const std::size_t total = kCarrierGuardBytes + limits.carrier_stack_bytes;
        void* stack = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        void* signal_stack = mmap(nullptr, kSignalStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        carrier->stack = stack == MAP_FAILED ? nullptr : stack;
        carrier->signal_stack = signal_stack == MAP_FAILED ? nullptr : signal_stack;
        carrier->stack_guard_low = static_cast<std::uint8_t*>(carrier->stack);
        core->carriers.push_back(std::move(carrier));
        if (stack == MAP_FAILED || signal_stack == MAP_FAILED || mprotect(stack, kCarrierGuardBytes, PROT_NONE) != 0)
            give_up("TenantVmHost: carrier stack");
    }
    // Every Carrier exists before any thread can scan the array.
    for (auto& carrier : core->carriers) {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstack(&attributes, static_cast<std::uint8_t*>(carrier->stack) + kCarrierGuardBytes,
                              limits.carrier_stack_bytes);
        struct Start { Core* core; Carrier* carrier; };
        auto* start = new Start{core, carrier.get()};
        const int rc = pthread_create(&carrier->thread, &attributes, [](void* raw) -> void* {
            auto* s = static_cast<Start*>(raw);
            Core* c = s->core;
            Carrier* self = s->carrier;
            delete s;
            c->carrier_main(self);
            return nullptr;
        }, start);
        pthread_attr_destroy(&attributes);
        if (rc != 0) {
            delete start;
            give_up("TenantVmHost: carrier thread");
        }
        carrier->started = true;
    }
    core->watchdog = std::thread([core] { core->watchdog_main(); });
}

TenantVmHost::~TenantVmHost() {
    std::vector<std::shared_ptr<Vm>> remaining;
    {
        std::lock_guard<std::mutex> lock(core_->registry_mutex);
        for (auto& entry : core_->by_generation) remaining.push_back(entry.second);
    }
    for (auto& vm : remaining) {
        core_->fail(vm, FailReason::HostClosed, 0);
        vm->closed.wait();
    }
    core_->watchdog_stop.store(true);
    core_->watchdog.join();
    core_->stop_carriers();
}

VmOpenResult TenantVmHost::open(VmSpec spec) {
    VmOpenResult out;
    const auto refuse = [&](Status status) { out.status = status; return out; };
    const VmLimits& l = spec.limits;
    if (!valid_tenant_id(spec.tenant_id) || !valid_module(spec.module) || l.heap_bytes == 0 ||
        l.heap_bytes > kAddressableBytes - (std::uint64_t{1} << 20) || l.max_actors == 0 ||
        l.mailbox_capacity == 0 || l.max_inflight == 0 || l.max_parallel_turns == 0 ||
        l.max_payload_bytes == 0 || l.max_reply_bytes == 0 || l.turn_deadline_ms == 0)
        return refuse(Status::InvalidArgument);
    if (!handlers_are_ours()) return refuse(Status::SignalHandlerLost);

    auto vm = std::make_shared<Vm>();
    vm->spec = std::move(spec);
    vm->closed = vm->closed_promise.get_future().share();
    {
        std::lock_guard<std::mutex> lock(core_->registry_mutex);
        // An id stays taken until its cage is gone, so a restart can never overlap the old VM.
        if (core_->by_id.count(vm->spec.tenant_id)) return refuse(Status::DuplicateTenant);
        if (core_->by_id.size() >= core_->limits.max_vms) return refuse(Status::TenantQuota);
        if (vm->spec.limits.heap_bytes > core_->limits.max_admitted_heap_bytes - core_->admitted_heap)
            return refuse(Status::HeapQuota);
        // Reserve only: no memory is committed and nothing is readable yet.
        void* cage = mmap(nullptr, kCageBytes, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (cage == MAP_FAILED) return refuse(Status::OutOfAddressSpace);
        vm->cage = static_cast<std::uint8_t*>(cage);
        vm->committed = Core::page_size();
        vm->bump = Core::page_size();
        vm->generation = core_->next_generation++;
        core_->admitted_heap += vm->spec.limits.heap_bytes;
        core_->by_id.emplace(vm->spec.tenant_id, vm);
        core_->by_generation.emplace(vm->generation, vm);
    }
    out.status = Status::Ok;
    out.handle.root_ = core_->root;
    out.handle.vm_ = vm;
    out.handle.generation_ = vm->generation;
    out.handle.closed_ = vm->closed;
    return out;
}

VmSpawnResult TenantVmHost::spawn_actor(const VmHandle& handle, std::uint32_t kind) {
    VmSpawnResult out;
    Status status = Status::Ok;
    auto vm = core_->resolve(handle.root_, handle.vm_, handle.generation_, status);
    if (!vm) { out.status = status; return out; }
    std::lock_guard<std::mutex> lock(vm->mutex);
    if (vm->current_phase() != Phase::Running) { out.status = Status::NotRunning; return out; }
    std::uint64_t ref = 0;
    out.status = core_->spawn_locked(vm, kind, ref);
    if (out.status == Status::Ok) {
        out.actor.root_ = handle.root_;
        out.actor.vm_ = handle.vm_;
        out.actor.generation_ = handle.generation_;
        out.actor.ref_ = ref;
    }
    return out;
}

std::future<TurnResult> TenantVmHost::call(const VmActor& ref, std::vector<std::uint8_t> payload) {
    Status status = Status::Ok;
    auto vm = core_->resolve(ref.root_, ref.vm_, ref.generation_, status);
    if (!vm) return ready_result(status);
    std::lock_guard<std::mutex> lock(vm->mutex);
    if (vm->current_phase() != Phase::Running) return ready_result(Status::NotRunning);
    Actor* actor = core_->resolve_actor_locked(*vm, ref.ref_);
    if (!actor) return ready_result(core_->was_terminated_locked(*vm, ref.ref_) ? Status::ActorTerminated : Status::UnknownActor);
    if (actor->stopping) return ready_result(Status::ActorStopping);
    if (payload.size() > Core::mail_capacity(*actor->kind, vm->spec.limits)) return ready_result(Status::PayloadTooLarge);
    if (actor->mailbox.size() >= vm->spec.limits.mailbox_capacity) return ready_result(Status::MailboxFull);
    if (vm->inflight >= vm->spec.limits.max_inflight) return ready_result(Status::Busy);
    if (!core_->charge_mail_locked(*vm, payload.size())) return ready_result(Status::Busy);
    Mail mail;
    mail.bytes = std::move(payload);
    mail.has_promise = true;
    auto future = mail.promise.get_future();
    actor->mailbox.push_back(std::move(mail));
    ++vm->inflight;
    core_->make_ready_locked(vm, static_cast<std::uint32_t>(ref.ref_));
    return future;
}

Status TenantVmHost::stop_actor(const VmActor& ref) {
    Status status = Status::Ok;
    auto vm = core_->resolve(ref.root_, ref.vm_, ref.generation_, status);
    if (!vm) return status;
    std::lock_guard<std::mutex> lock(vm->mutex);
    if (vm->current_phase() != Phase::Running) return Status::NotRunning;
    Actor* actor = core_->resolve_actor_locked(*vm, ref.ref_);
    if (!actor) return core_->was_terminated_locked(*vm, ref.ref_) ? Status::ActorTerminated : Status::UnknownActor;
    if (actor->stopping) return Status::ActorStopping;
    actor->stopping = true;
    if (!actor->running && actor->mailbox.empty())
        core_->release_actor_locked(*vm, *actor, static_cast<std::uint32_t>(ref.ref_));
    return Status::Ok;
}

Status TenantVmHost::begin_shutdown(const VmHandle& handle) {
    Status status = Status::Ok;
    auto vm = core_->resolve(handle.root_, handle.vm_, handle.generation_, status);
    if (!vm) return status;
    {
        std::lock_guard<std::mutex> lock(vm->mutex);
        if (vm->current_phase() != Phase::Running) return Status::NotRunning;
        vm->phase.store(static_cast<int>(Phase::Quiescing), std::memory_order_seq_cst);
    }
    core_->maybe_retire(vm);
    return Status::Ok;
}

Status TenantVmHost::kill(const VmHandle& handle) {
    Status status = Status::Ok;
    auto vm = core_->resolve(handle.root_, handle.vm_, handle.generation_, status);
    if (!vm) return status;
    core_->fail(vm, FailReason::HostKill, 0);
    return Status::Ok;
}

VmTerminal TenantVmHost::wait_closed(const VmHandle& handle) const {
    if (handle.root_ != core_->root || !handle.closed_.valid()) {
        VmTerminal terminal;
        terminal.detail = "foreign or empty handle";
        return terminal;
    }
    return handle.closed_.get();
}

VmSnapshot TenantVmHost::snapshot(const VmHandle& handle) const {
    VmSnapshot out;
    Status status = Status::Ok;
    auto vm = core_->resolve(handle.root_, handle.vm_, handle.generation_, status);
    out.status = status;
    if (!vm) return out;
    std::lock_guard<std::mutex> lock(vm->mutex);
    out.phase = vm->current_phase();
    out.generation = vm->generation;
    out.committed_bytes = vm->committed;
    out.used_bytes = vm->used;
    out.turns = vm->turns;
    out.cpu_ns = vm->cpu_ns.load(std::memory_order_relaxed);
    out.live_actors = vm->live_actors;
    out.inflight = vm->inflight;
    out.actor_restarts = vm->actor_restarts;
    out.mail_lost = vm->mail_lost;
    out.actors_terminated = vm->actors_terminated;
    out.mail_abandoned = vm->mail_abandoned;
    return out;
}

HostStats TenantVmHost::stats() const {
    HostStats out;
    std::lock_guard<std::mutex> lock(core_->registry_mutex);
    out.live_vms = core_->by_id.size();
    out.admitted_heap_bytes = core_->admitted_heap;
    out.traps_contained = core_->traps_contained.load();
    out.kills_delivered = core_->kills_delivered.load();
    out.wild_accesses = core_->wild_accesses.load();
    out.queued_bytes = core_->queued_bytes.load();
    out.actor_restarts = core_->actor_restarts.load();
    return out;
}

} // namespace oreslang::runtime::vm
