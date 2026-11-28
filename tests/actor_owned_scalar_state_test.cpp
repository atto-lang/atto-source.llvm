#include "oreslang/runtime/ActorOwnedScalarState.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace oreslang;
static void check(bool ok) { if (!ok) throw std::runtime_error("scalar state check failed"); }

template <class Fn> static void rejects(Fn&& fn) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    check(failed);
}

int main() {
    using State = ActorOwnedScalarState;
    const std::uint64_t actor = 42;
    State state(actor, {
        {"count", {State::Type::Int, std::int64_t{0}}},
        {"enabled", {State::Type::Bool, true}},
        {"label", {State::Type::String, std::string("worker")}}
    });
    CriticalStateCallback checked{};
    checked.callback_critical = true;
    checked.one_state_parameter = true;
    checked.inferred = {static_cast<std::uint32_t>(FunctionEffect::StateRead)
            | static_cast<std::uint32_t>(FunctionEffect::StateWrite)};
    const FunctionEffects allowed = checked.inferred;
    const FunctionEffects readonly{static_cast<std::uint32_t>(FunctionEffect::StateRead)};

    check(state.ready_to_suspend(actor));
    // The serial guarantee is stored on the actor's runtime instance, not
    // supplied by a guest callback. Per spec ch.05/ch.22 it does not waive
    // explicit critical marking for state writes: an unmarked callback is
    // READ-only on serial and ordinary actors alike.
    State serial_actor(actor + 1, {
        {"count", {State::Type::Int, std::int64_t{0}}}
    }, true);
    auto plain = checked;
    plain.callback_critical = false;
    rejects([&] {
        (void) serial_actor.critical(actor + 1, 299, plain, allowed,
                [](State::Access&) -> State::Value { return std::int64_t{0}; });
    });
    auto plain_read = plain;
    plain_read.inferred = readonly;
    check(std::get<std::int64_t>(serial_actor.critical(
            actor + 1, 300, plain_read, readonly,
            [](State::Access& access) -> State::Value {
        return access.read("count");
    })) == 0);
    auto incremented = serial_actor.critical(actor + 1, 301, checked, allowed,
            [&](State::Access& access) -> State::Value {
        const auto count = std::get<std::int64_t>(access.read("count"));
        access.write("count", count + 1);
        return access.read("count");
    });
    check(std::get<std::int64_t>(incremented) == 1);
    // Explicit critical continues to be legal on serial actors.
    check(std::get<std::int64_t>(serial_actor.critical(
            actor + 1, 302, checked, allowed,
            [](State::Access& access) -> State::Value {
        return access.read("count");
    })) == 1);
    rejects([&] {
        (void) state.critical(actor, 300, plain, allowed,
                [](State::Access&) -> State::Value { return std::int64_t{0}; });
    });
    for (int i = 0; i < 100; ++i) {
        const auto next = state.critical(actor, 100, checked, allowed,
                [&](State::Access& access) -> State::Value {
            check(!state.ready_to_suspend(actor));
            const auto n = std::get<std::int64_t>(access.read("count"));
            access.write("count", n + 1);
            if (i == 0) {
                rejects([&] {
                    (void) state.critical(actor, 101, checked, allowed,
                            [&](State::Access&) -> State::Value {
                                return std::int64_t{999};
                            });
                });
                // Sealed critical sections never nest, not even on the same
                // continuation (68474643b): a nested read/read or read/write
                // loan would re-enter state authority inside the caller's
                // single-transaction effect proof. The rejected admission
                // must leave the outer loan valid and unchanged.
                rejects([&] {
                    (void) state.critical(actor, 100, checked, allowed,
                            [&](State::Access&) -> State::Value {
                                return std::int64_t{999};
                            });
                });
                check(std::get<std::int64_t>(access.read("count")) == 1);
            }
            return access.read("count");
        });
        check(std::get<std::int64_t>(next) == i + 1);
    }
    check(state.ready_to_suspend(actor));
    check(std::get<std::int64_t>(state.critical(actor, 201, checked, allowed,
            [&](State::Access& a) -> State::Value { return a.read("count"); })) == 100);

    auto underdeclared = checked;
    rejects([&] {
        (void) state.critical(actor, 202, underdeclared, readonly,
                [](State::Access&) -> State::Value { return std::int64_t{0}; });
    });
    rejects([&] {
        (void) state.critical(99, 202, checked, allowed,
                [](State::Access&) -> State::Value { return std::int64_t{0}; });
    });
    rejects([&] {
        (void) state.critical(actor, 203, checked, allowed,
                [](State::Access& access) -> State::Value {
            access.write("count", std::string("wrong type"));
            return std::int64_t{0};
        });
    });
    rejects([&] {
        (void) state.critical(actor, 204, checked, allowed,
                [](State::Access& access) -> State::Value {
            return access.read("unknown");
        });
    });
    try {
        (void) state.critical(actor, 205, checked, allowed,
                [](State::Access&) -> State::Value {
            throw std::runtime_error("guest throw unwinds native critical loan");
        });
    } catch (const std::runtime_error&) { }
    check(state.ready_to_suspend(actor));
    rejects([&] {
        (void) state.critical(actor, 207, checked, allowed,
                [](State::Access& access) -> State::Value {
            access.write("label", std::string(64 * 1024 + 1, 'x'));
            return std::int64_t{0};
        });
    });
    rejects([&] {
        State oversize(actor, {
            {"label", {State::Type::String, std::string(64 * 1024 + 1, 'x')}}
        });
        (void) oversize;
    });
    // Four independent native actor-owned turns produce the same Counter=10
    // contract as the Java checked-state primitive. These are real checked
    // native loans; they are not yet LLVM guest source-level state lowering.
    State left(actor + 40, {{"total", {State::Type::Int, std::int64_t{0}}}});
    State right(actor + 41, {{"total", {State::Type::Int, std::int64_t{0}}}});
    std::int64_t left_sum = 0;
    for (std::int64_t delta : {1, 2, 3, 4}) {
        left_sum += delta;
        const auto observed = left.critical(actor + 40, 710, checked, allowed,
                [&](State::Access& view) -> State::Value {
            check(!left.ready_to_suspend(actor + 40));
            return view.add_checked("total", delta);
        });
        check(std::get<std::int64_t>(observed) == left_sum);
        check(left.ready_to_suspend(actor + 40));
    }
    check(left_sum == 10);
    check(std::get<std::int64_t>(right.critical(actor + 41, 711, checked, allowed,
            [](State::Access& view) -> State::Value {
        return view.add_checked("total", 7);
    })) == 7);
    // A different actor's state is private, not an alias of left.total.
    check(std::get<std::int64_t>(left.critical(actor + 40, 712, checked, allowed,
            [](State::Access& view) -> State::Value { return view.read("total"); })) == 10);
    check(std::get<std::int64_t>(right.critical(actor + 41, 712, checked, allowed,
            [](State::Access& view) -> State::Value { return view.read("total"); })) == 7);
    rejects([&] {
        (void) left.critical(actor + 41, 713, checked, allowed,
                [](State::Access& view) -> State::Value {
            return view.add_checked("total", 1);
        });
    });
    rejects([&] {
        (void) left.critical(actor + 40, 714, checked, readonly,
                [](State::Access& view) -> State::Value {
            return view.add_checked("total", 1);
        });
    });

    State positive_limit(actor + 42, {{"total", {State::Type::Int,
            std::numeric_limits<std::int64_t>::max()}}});
    rejects([&] {
        (void) positive_limit.critical(actor + 42, 715, checked, allowed,
                [](State::Access& view) -> State::Value {
            return view.add_checked("total", 1);
        });
    });
    check(positive_limit.ready_to_suspend(actor + 42));
    check(std::get<std::int64_t>(positive_limit.critical(
            actor + 42, 716, checked, allowed,
            [](State::Access& view) -> State::Value {
                return view.read("total");
            })) == std::numeric_limits<std::int64_t>::max());
    State negative_limit(actor + 43, {{"total", {State::Type::Int,
            std::numeric_limits<std::int64_t>::min()}}});
    rejects([&] {
        (void) negative_limit.critical(actor + 43, 717, checked, allowed,
                [](State::Access& view) -> State::Value {
            return view.add_checked("total", -1);
        });
    });
    check(std::get<std::int64_t>(negative_limit.critical(
            actor + 43, 718, checked, allowed,
            [](State::Access& view) -> State::Value { return view.read("total"); }))
            == std::numeric_limits<std::int64_t>::min());
    left.retire(actor + 40);
    right.retire(actor + 41);
    positive_limit.retire(actor + 42);
    negative_limit.retire(actor + 43);

    rejects([&] { state.retire(99); });
    state.retire(actor);
    rejects([&] {
        (void) state.critical(actor, 206, checked, allowed,
                [](State::Access&) -> State::Value { return std::int64_t{0}; });
    });
    return 0;
}
