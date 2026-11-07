#include "oreslang/runtime/ActorGuestFrameStack.hpp"
#include "oreslang/runtime/MultiActorCarrier.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "actor guest stack failure %s:%d: %s\n", \
                 __FILE__, __LINE__, #value); std::abort(); \
} } while (false)

template<typename Condition>
static bool wait_until(Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

static void test_bounded_frame_model() {
    ActorGuestFrameStack a, b;
    CHECK(a.initialize(1001));
    CHECK(b.initialize(1002));
    CHECK(!a.initialize(1003));
    ActorGuestFrameStack::FrameToken a_token, b_token;
    CHECK(a.push(sizeof(std::uint64_t), a_token));
    CHECK(b.push(sizeof(std::uint64_t), b_token));
    const std::uint64_t first = 0x1122334455667788ULL;
    std::uint64_t observed = 0;
    CHECK(a.write(a_token, 0, &first, sizeof(first)));
    CHECK(a.read(a_token, 0, &observed, sizeof(observed)));
    CHECK(observed == first);
    CHECK(!b.read(a_token, 0, &observed, sizeof(observed)));
    CHECK(!a.write(b_token, 0, &first, sizeof(first)));
    CHECK(!a.read(a_token, SIZE_MAX, &observed, sizeof(observed)));
    CHECK(!a.write(a_token, 1, &first, sizeof(first)));
    CHECK(!a.push(0, b_token));
    CHECK(!a.push(ActorGuestFrameStack::max_frame_bytes + 1, b_token));
    CHECK(a.pop(a_token));
    CHECK(!a.read(a_token, 0, &observed, sizeof(observed)));
    ActorGuestFrameStack::FrameToken fresh;
    CHECK(a.push(sizeof(observed), fresh));
    CHECK(fresh.serial != a_token.serial);
    observed = 1;
    CHECK(a.read(fresh, 0, &observed, sizeof(observed)));
    CHECK(observed == 0); // physical frame bytes scrubbed between messages
    CHECK(a.pop(fresh));
    std::array<ActorGuestFrameStack::FrameToken, ActorGuestFrameStack::max_depth> tokens{};
    for (auto& token : tokens) CHECK(a.push(128, token));
    CHECK(a.depth() == ActorGuestFrameStack::max_depth);
    CHECK(a.used_bytes() == ActorGuestFrameStack::capacity_bytes);
    CHECK(!a.push(8, fresh));
    CHECK(!a.pop(tokens[0])); // cannot pop non-top frame
    for (std::size_t i = tokens.size(); i != 0; --i) CHECK(a.pop(tokens[i - 1]));
    CHECK(a.depth() == 0 && a.used_bytes() == 0);
    a.retire();
    CHECK(!a.push(8, fresh));
    CHECK(!a.read(a_token, 0, &observed, sizeof(observed)));
    CHECK(!a.initialize(1001)); // retired incarnation never reissues authority
}

struct Shared {
    MultiActorCarrier::ActorToken a_id{}, b_id{};
    MultiActorCarrier::GuestFrameToken a_frame{};
    std::atomic<int> a_initial{0}, a_resumes{0}, b_done{0}, a_cleanup{0};
    std::atomic<int> b_cleanup{0}, failures{0};
};
struct State { Shared* shared; bool a; };

static MultiActorCarrier::Outcome slice(void* raw, MultiActorCarrier::Message msg,
        MultiActorCarrier::Cause cause, std::uint64_t) noexcept {
    auto& state = *static_cast<State*>(raw);
    auto& shared = *state.shared;
    const auto owner = state.a ? shared.a_id : shared.b_id;
    std::uint64_t value = 0;
    if (msg.sequence != 1) shared.failures.fetch_add(1);
    if (state.a) {
        if (cause == MultiActorCarrier::Cause::initial) {
            if (!MultiActorCarrier::guest_frame_push(owner, sizeof(value), shared.a_frame))
                shared.failures.fetch_add(1);
            value = 37;
            if (!MultiActorCarrier::guest_frame_write(
                    owner, shared.a_frame, 0, &value, sizeof(value)))
                shared.failures.fetch_add(1);
            shared.a_initial.fetch_add(1, std::memory_order_release);
        } else {
            if (!MultiActorCarrier::guest_frame_read(
                    owner, shared.a_frame, 0, &value, sizeof(value)) || value != 37)
                shared.failures.fetch_add(1);
            shared.a_resumes.fetch_add(1);
        }
        // Worker releases its PHYSICAL stack, while this actor's logical
        // frame persists. Another actor must progress on the same worker.
        return MultiActorCarrier::Outcome::cooperate;
    }
    if (cause != MultiActorCarrier::Cause::initial)
        shared.failures.fetch_add(1);
    if (MultiActorCarrier::guest_frame_read(shared.a_id, shared.a_frame,
                                            0, &value, sizeof(value)))
        shared.failures.fetch_add(1); // sibling cannot impersonate actor A
    if (MultiActorCarrier::guest_frame_read(owner, shared.a_frame,
                                            0, &value, sizeof(value)))
        shared.failures.fetch_add(1); // cross-incarnation frame is invalid
    MultiActorCarrier::GuestFrameToken b_frame;
    if (!MultiActorCarrier::guest_frame_push(owner, sizeof(value), b_frame))
        shared.failures.fetch_add(1);
    value = 91;
    if (!MultiActorCarrier::guest_frame_write(owner, b_frame, 0, &value, sizeof(value)))
        shared.failures.fetch_add(1);
    value = 0;
    if (!MultiActorCarrier::guest_frame_read(owner, b_frame,
            0, &value, sizeof(value)) || value != 91)
        shared.failures.fetch_add(1);
    if (!MultiActorCarrier::guest_frame_pop(owner, b_frame))
        shared.failures.fetch_add(1);
    shared.b_done.fetch_add(1, std::memory_order_release);
    return MultiActorCarrier::Outcome::complete;
}
static void cleanup(void* raw, MultiActorCarrier::Message,
                    bool had_active) noexcept {
    auto& state = *static_cast<State*>(raw);
    auto& shared = *state.shared;
    const auto owner = state.a ? shared.a_id : shared.b_id;
    if (state.a) {
        std::uint64_t value = 0;
        if (!had_active || !MultiActorCarrier::guest_frame_read(
                owner, shared.a_frame, 0, &value, sizeof(value)) || value != 37 ||
                !MultiActorCarrier::guest_frame_pop(owner, shared.a_frame))
            shared.failures.fetch_add(1);
        shared.a_cleanup.fetch_add(1);
    } else {
        if (had_active || MultiActorCarrier::guest_frame_depth(owner) != 0)
            shared.failures.fetch_add(1);
        shared.b_cleanup.fetch_add(1);
    }
}

static void test_carrier_owned_frames() {
    Shared shared;
    State a{&shared, true}, b{&shared, false};
    MultiActorCarrier carrier(32);
    shared.a_id = carrier.register_actor(&a, slice, cleanup);
    shared.b_id = carrier.register_actor(&b, slice, cleanup);
    MultiActorCarrier::GuestFrameToken outsider;
    CHECK(!MultiActorCarrier::guest_frame_push(shared.a_id, 8, outsider));
    CHECK(outsider.serial == 0);
    auto producer = carrier.producer();
    carrier.start();
    CHECK(producer.try_send(shared.a_id, {1, 37}));
    CHECK(wait_until([&] { return shared.a_initial.load() == 1; }));
    CHECK(producer.try_send(shared.b_id, {1, 91}));
    CHECK(wait_until([&] { return carrier.actor_stats(shared.b_id).completed == 1; }));
    CHECK(wait_until([&] { return shared.a_resumes.load() >= 1; }));
    CHECK(producer.request_cancel(shared.a_id));
    CHECK(wait_until([&] { return carrier.actor_stats(shared.a_id).retired; }));
    carrier.join();
    CHECK(shared.a_cleanup.load() == 1);
    CHECK(shared.b_cleanup.load() == 1);
    CHECK(shared.b_done.load() == 1);
    CHECK(shared.failures.load() == 0);
    CHECK(carrier.stats().protocol_failures == 0);
    CHECK(carrier.stats().accepted == 2 && carrier.stats().completed == 1);
    CHECK(!MultiActorCarrier::guest_frame_read(shared.a_id, shared.a_frame,
                                               0, &outsider, sizeof(outsider)));
    CHECK(!producer.try_send(shared.a_id, {2, 0}));
    CHECK(!producer.try_send(shared.b_id, {2, 0}));
}

int main() {
    test_bounded_frame_model();
    test_carrier_owned_frames();
    std::puts("ACTOR_OWNED_LOGICAL_FRAME_STORE_HOST_PASS");
}
