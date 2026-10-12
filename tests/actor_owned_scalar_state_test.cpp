#include "oreslang/runtime/ActorOwnedScalarState.hpp"

#include <cassert>
#include <cstdint>
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
    // supplied by a guest callback. It admits the optional critical marker.
    State serial_actor(actor + 1, {
        {"count", {State::Type::Int, std::int64_t{0}}}
    }, true);
    auto plain = checked;
    plain.callback_critical = false;
    auto incremented = serial_actor.critical(actor + 1, 300, plain, allowed,
            [&](State::Access& access) -> State::Value {
        const auto count = std::get<std::int64_t>(access.read("count"));
        access.write("count", count + 1);
        return access.read("count");
    });
    check(std::get<std::int64_t>(incremented) == 1);
    // Explicit critical continues to be legal on serial actors.
    check(std::get<std::int64_t>(serial_actor.critical(
            actor + 1, 301, checked, allowed,
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
                const auto nested = state.critical(actor, 100, checked, allowed,
                        [&](State::Access& inner) -> State::Value {
                    rejects([&] { (void) access.read("count"); });
                    check(std::get<std::int64_t>(inner.read("count")) == 1);
                    return inner.read("count");
                });
                check(std::get<std::int64_t>(nested) == 1);
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
    rejects([&] { state.retire(99); });
    state.retire(actor);
    rejects([&] {
        (void) state.critical(actor, 206, checked, allowed,
                [](State::Access&) -> State::Value { return std::int64_t{0}; });
    });
    return 0;
}
