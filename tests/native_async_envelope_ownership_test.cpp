#include "oreslang/runtime/NativeAsyncEnvelopeTxn.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeAsyncEnvelopeTxn;
using oreslang::runtime::NativeAsyncMailboxAdmission;
using oreslang::runtime::PriorityActorInbox;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("native async envelope ownership: " #x); } while (false)

struct Payload {
    int value = 0;
};

struct WakeFixture {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef producer{};
    OresNativeActorRef target{};
    std::atomic<int> ready{0};
    std::atomic<int> failed{0};
    std::atomic<int> errors{0};
};

static void on_data(void*, std::size_t, std::uint64_t) {}

static void on_wake(
        void* raw, std::size_t actor, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    auto* f = static_cast<WakeFixture*>(raw);
    if (actor != 0
            || !ores_native_actor_scheduler_is_current_actor(
                f->scheduler, f->producer)
            || !ores_native_actor_scheduler_is_current_wake(
                f->scheduler, ticket))
        ++f->errors;
    if (outcome == ORES_NATIVE_WAKE_READY) ++f->ready;
    else if (outcome == ORES_NATIVE_WAKE_FAILED) ++f->failed;
    else ++f->errors;
}

static void setup_scheduler(WakeFixture& f) {
    f.scheduler = ores_native_actor_scheduler_open_with_wakes(
        2, 1, 1, on_data, nullptr, nullptr, on_wake, &f);
    CHECK(f.scheduler);
    f.producer = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    f.target = ores_native_actor_scheduler_spawn(f.scheduler, 1);
    CHECK(f.producer.scheduler_domain && f.target.scheduler_domain);
}

static void finish_scheduler(WakeFixture& f, bool target_already_stopped = false) {
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(!f.errors.load());
    if (!target_already_stopped)
        CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.producer));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.producer));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

using Inbox = PriorityActorInbox<Payload>;
using Txn = NativeAsyncEnvelopeTxn<Payload>;
using Lane = Inbox::Lane;

static bool sync_offer_owned(
        NativeAsyncMailboxAdmission& admission,
        Inbox& inbox,
        const oreslang::runtime::NativeActorRef& target,
        std::unique_ptr<Payload>& payload,
        std::size_t bytes) {
    if (!admission.try_acquire(bytes)) return false;
    try {
        if (inbox.offer_owned_to(target, Lane::Normal, payload, bytes))
            return true;
    } catch (...) {
        admission.release_acquired(bytes);
        throw;
    }
    admission.release_acquired(bytes);
    return false;
}

static void waiting_send_transfers_frozen_payload_and_exact_resource_loan() {
    NativeActorGroup group(7001, 1);
    auto target = group.adopt(7001, 41, 1, [] {});
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 4, 32);

    WakeFixture f;
    setup_scheduler(f);

    // Occupy the real and shadow NORMAL credit with an ordinary accepted send.
    auto blocker = std::make_unique<Payload>();
    blocker->value = 1;
    CHECK(sync_offer_owned(admission, inbox, target, blocker, 8));
    CHECK(!blocker);
    CHECK(target.outstanding_resource_loans() == 1);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));

    // Preparation is bounded before the potentially expensive copy. Mutating
    // the sender afterwards must not change the runtime-owned frozen snapshot.
    Payload source{7};
    auto frozen = std::make_unique<Payload>(source);
    source.value = 99;

    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        101, 1, 1) == NativeAsyncMailboxAdmission::Registration::Waiting);
    CHECK(!frozen);
    CHECK(txn.owns_payload());
    CHECK(txn.owns_target_loan());
    CHECK(target.outstanding_resource_loans() == 2);
    CHECK(admission.pending_waiters() == 1);
    CHECK(admission.pending_bytes() == 8);

    auto first = inbox.try_receive();
    CHECK(first && first->value().value == 1);
    CHECK(first->acknowledge_released());
    CHECK(target.outstanding_resource_loans() == 1);

    // Physical ACK frees the real port first; releasing the mirrored async
    // credit then transfers that exact slot to the FIFO waiter.
    admission.release_acquired(8);
    CHECK(admission.transferred(handle));
    CHECK(admission.used_frames() == 1);
    CHECK(admission.pending_waiters() == 0);
    CHECK(target.outstanding_resource_loans() == 1);

    CHECK(txn.publish_if_transferred(inbox) == Txn::PublishResult::Published);
    CHECK(!txn.owns_payload());
    CHECK(!txn.owns_target_loan());
    // Loan ownership moved from txn to the actual envelope, so count is stable.
    CHECK(target.outstanding_resource_loans() == 1);

    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 1);
    CHECK(f.failed.load() == 0);

    auto delivered = inbox.try_receive();
    CHECK(delivered);
    CHECK(delivered->value().value == 7);
    CHECK(delivered->acknowledge_released());
    CHECK(target.outstanding_resource_loans() == 0);
    admission.release_acquired(8);
    CHECK(admission.used_frames() == 0);
    CHECK(admission.active_descriptors() == 0);
    CHECK(admission.pending_bytes() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    inbox.close();
    finish_scheduler(f);
}

static void waiting_cancellation_releases_payload_loan_and_wake() {
    NativeActorGroup group(7002, 1);
    auto target = group.adopt(7002, 42, 1, [] {});
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);
    WakeFixture f;
    setup_scheduler(f);

    auto blocker = std::make_unique<Payload>();
    blocker->value = 2;
    CHECK(sync_offer_owned(admission, inbox, target, blocker, 8));

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 8;
    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        201, 1, 1) == NativeAsyncMailboxAdmission::Registration::Waiting);
    CHECK(target.outstanding_resource_loans() == 2);
    CHECK(txn.cancel_waiting());
    CHECK(!txn.owns_payload());
    CHECK(!txn.owns_target_loan());
    CHECK(target.outstanding_resource_loans() == 1);
    CHECK(admission.pending_waiters() == 0);
    CHECK(admission.active_descriptors() == 0);

    auto first = inbox.try_receive();
    CHECK(first && first->acknowledge_released());
    admission.release_acquired(8);
    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 0 && f.failed.load() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    inbox.close();
    finish_scheduler(f);
}

static void publication_exception_aborts_transfer_and_releases_target_loan() {
    NativeActorGroup group(7003, 1);
    auto target = group.adopt(7003, 43, 1, [] {});
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);
    WakeFixture f;
    setup_scheduler(f);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 9;
    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        301, 1, 1) == NativeAsyncMailboxAdmission::Registration::Transferred);
    CHECK(target.outstanding_resource_loans() == 1);
    CHECK(admission.used_frames() == 1);

    // No real envelope exists yet, so sealing the underlying inbox succeeds.
    // Publication must then roll back transferred wake/credit + target loan.
    inbox.close();
    bool threw = false;
    try { (void)txn.publish_if_transferred(inbox); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
    CHECK(!txn.owns_payload());
    CHECK(!txn.owns_target_loan());
    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(admission.used_frames() == 0);
    CHECK(admission.active_descriptors() == 0);
    CHECK(admission.pending_bytes() == 0);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 0 && f.failed.load() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    finish_scheduler(f);
}

static void producer_stop_after_transfer_does_not_retract_envelope() {
    NativeActorGroup group(7004, 1);
    auto target = group.adopt(7004, 44, 1, [] {});
    CHECK(group.mark_ready(target));

    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 8);
    inbox.bind_target(target);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);
    WakeFixture f;
    setup_scheduler(f);

    NativeAsyncMailboxAdmission::Handle handle;
    CHECK(admission.begin_preparation(&handle));
    auto frozen = std::make_unique<Payload>();
    frozen->value = 10;
    Txn txn(admission, handle, target, Lane::Normal, 8);
    CHECK(txn.reserve_and_arm_owned(
        frozen, f.scheduler, f.producer, f.target,
        401, 1, 1) == NativeAsyncMailboxAdmission::Registration::Transferred);

    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.producer));
    CHECK(txn.publish_if_transferred(inbox)
          == Txn::PublishResult::ProducerRetired);
    CHECK(target.outstanding_resource_loans() == 1);

    auto delivered = inbox.try_receive();
    CHECK(delivered && delivered->value().value == 10);
    CHECK(delivered->acknowledge_released());
    admission.release_acquired(8);
    CHECK(target.outstanding_resource_loans() == 0);
    CHECK(f.ready.load() == 0 && f.failed.load() == 0);

    CHECK(group.finalized(target));
    target.get_done().get();
    inbox.close();

    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.producer));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

int main() {
    waiting_send_transfers_frozen_payload_and_exact_resource_loan();
    waiting_cancellation_releases_payload_loan_and_wake();
    publication_exception_aborts_transfer_and_releases_target_loan();
    producer_stop_after_transfer_does_not_retract_envelope();
    return 0;
}
