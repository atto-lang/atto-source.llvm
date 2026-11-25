#include "oreslang/runtime/NativeSharedPayloadSchedulerBinding.hpp"

#include <atomic>
#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace oreslang::runtime;

struct Context final {
    OresNativeActorScheduler* scheduler = nullptr;
    NativeSharedPayloadSchedulerBinding binding{};
    SharedImmutablePayload published{};
    std::atomic<unsigned> completed{0};
};

static int forged_commit_calls = 0;
static int forged_commit(void*, std::uint64_t) {
    ++forged_commit_calls;
    return 1;
}

static void on_data(void* opaque, std::size_t actor_index, std::uint64_t message) {
    auto& ctx = *static_cast<Context*>(opaque);
    assert(actor_index == 0);
    const auto expected = ctx.completed.load(std::memory_order_acquire);
    assert(expected < 2 && message == 700 + expected);

    std::uint64_t ordinal = 0;
    assert(ores_native_actor_scheduler_current_data_mail_sequence(
        ctx.scheduler, ctx.binding.actor, &ordinal));
    assert(ordinal == expected + 1);
    auto wrong = ctx.binding.inbox->try_receive_sequenced(ordinal + 1);
    assert(wrong.status == SharedPayloadInbox::ReceiveStatus::denied);
    assert(!wrong.descriptor);

    auto got = native_shared_payload_receive_current_data(ctx.binding);
    assert(got.status == SharedPayloadInbox::ReceiveStatus::received);
    assert(got.descriptor && got.descriptor->same_backing(ctx.published));
    assert(got.descriptor->read_byte(0) == 42);
    assert(got.descriptor->size() == (expected == 0 ? 1024u : 3u));
    ctx.completed.fetch_add(1, std::memory_order_release);
}

int main() {
    auto published = SharedImmutablePayload::publish(
        std::vector<std::uint8_t>(1024, 42));
    auto slice = published.slice(4, 3);
    assert(slice && slice->same_backing(published));

    SharedPayloadInbox unbound(1, 1024);
    assert(unbound.try_send(published) == SharedPayloadInbox::SendResult::admitted);
    assert(!unbound.try_bind_sequenced());
    assert(unbound.try_receive_result().status
        == SharedPayloadInbox::ReceiveStatus::received);
    assert(unbound.try_bind_sequenced());
    assert(!unbound.try_bind_sequenced()); // Irreversible.
    assert(unbound.try_send(published) == SharedPayloadInbox::SendResult::invalid);
    assert(unbound.try_receive_result().status
        == SharedPayloadInbox::ReceiveStatus::denied);
    assert(unbound.try_send_sequenced(published, 0)
        == SharedPayloadInbox::SendResult::invalid);
    assert(unbound.try_close());

    Context ctx;
    ctx.published = published;
    ctx.scheduler = ores_native_actor_scheduler_open(1, 4, 1, on_data, &ctx);
    assert(ctx.scheduler);
    auto actor = ores_native_actor_scheduler_spawn(ctx.scheduler, 0);
    assert(actor.scheduler_domain && actor.generation);

    SharedPayloadInbox inbox(2, 2048);
    assert(native_shared_payload_bind(
        &ctx.binding, ctx.scheduler, actor, &inbox));
    NativeSharedPayloadSchedulerBinding duplicate{};
    assert(!native_shared_payload_bind(
        &duplicate, ctx.scheduler, actor, &inbox));
    assert(inbox.try_send(published) == SharedPayloadInbox::SendResult::invalid);
    assert(native_shared_payload_receive_current_data(ctx.binding).status
        == SharedPayloadInbox::ReceiveStatus::denied); // External caller.
    assert(ores_native_actor_scheduler_try_send(ctx.scheduler, actor, 700)
        == ORES_NATIVE_SEND_WRONG_MAIL_KIND);
    assert(ores_native_actor_scheduler_try_send_committed(
        ctx.scheduler, actor, 700, forged_commit, nullptr)
        == ORES_NATIVE_SEND_WRONG_MAIL_KIND);
    assert(forged_commit_calls == 0);

    auto oversized = SharedImmutablePayload::publish(
        std::vector<std::uint8_t>(4096, 9));
    assert(native_shared_payload_try_send_atomic(ctx.binding, 700, oversized)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    assert(inbox.queued() == 0 && inbox.charged_bytes() == 0);
    NativeSharedPayloadSchedulerBinding stale = ctx.binding;
    ++stale.actor.generation;
    assert(native_shared_payload_try_send_atomic(stale, 700, published)
        == ORES_NATIVE_SEND_INVALID_REF);
    assert(inbox.queued() == 0 && inbox.charged_bytes() == 0);

    assert(native_shared_payload_try_send_atomic(ctx.binding, 700, published)
        == ORES_NATIVE_SEND_ACCEPTED);
    assert(native_shared_payload_try_send_atomic(ctx.binding, 701, *slice)
        == ORES_NATIVE_SEND_ACCEPTED);

    for (unsigned i = 0; i < 3000 &&
         ctx.completed.load(std::memory_order_acquire) != 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Host test only.
    assert(ctx.completed.load(std::memory_order_acquire) == 2);
    assert(inbox.queued() == 0 && inbox.charged_bytes() == 0);

    assert(ores_native_actor_scheduler_stop(ctx.scheduler, actor));
    assert(ores_native_actor_scheduler_wait_done(ctx.scheduler, actor));
    OresNativeActorTerminalState terminal{};
    assert(ores_native_actor_scheduler_terminal_state(
        ctx.scheduler, actor, &terminal));
    assert(terminal.accepted_user_messages == 2);
    assert(terminal.processed_user_messages == 2);
    assert(terminal.abandoned_user_messages == 0);
    assert(inbox.try_close()); // STOP/DONE before physical queue teardown.
    assert(inbox.try_receive_sequenced(1).status
        == SharedPayloadInbox::ReceiveStatus::closed);
    assert(ores_native_actor_scheduler_poll_done(ctx.scheduler, actor));
    ores_native_actor_scheduler_retire(ctx.scheduler);
    assert(ores_native_actor_scheduler_drain(ctx.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(ctx.scheduler));
    assert(published.read_byte(0) == 42); // Shared backing outlives inbox.
    std::puts("NATIVE_SHARED_PAYLOAD_ATOMIC_HOST_ONLY_PASS actor=1 admissions=2 "
              "zero_copy_backing=1 ordinal_fencing=1 denied_scalar=1 "
              "denied_forged_callback=1 denied_stale_generation=1 "
              "failed_admission_rollback=1");
    return 0;
}
