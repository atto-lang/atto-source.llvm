#include "oreslang/runtime/ActorOwnedScalarState.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace oreslang;
static void check_impl(bool ok, const char* expression, int line) {
    if (!ok) throw std::runtime_error(std::string("scalar state check failed at ")
            + std::to_string(line) + ": " + expression);
}
#define check(condition) check_impl((condition), #condition, __LINE__)

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
    checked.state_mode = CriticalStateMode::Write;
    checked.one_state_parameter = true;
    checked.inferred = {static_cast<std::uint32_t>(FunctionEffect::StateRead)
            | static_cast<std::uint32_t>(FunctionEffect::StateWrite)};
    const FunctionEffects allowed = checked.inferred;
    const FunctionEffects readonly{static_cast<std::uint32_t>(FunctionEffect::StateRead)};

    check(state.ready_to_suspend(actor));
    // Serial admission never silently upgrades a plain read callback to
    // mutable authority. The runtime Access.write guard is independent of
    // the declared effect row.
    State serial_actor(actor + 1, {
        {"count", {State::Type::Int, std::int64_t{0}}}
    }, true);
    auto plain = checked;
    plain.callback_critical = false;
    plain.state_mode = CriticalStateMode::Read;
    plain.inferred = readonly;
    check(std::get<std::int64_t>(serial_actor.critical(
            actor + 1, 300, plain, readonly,
            [](State::Access& access) -> State::Value {
        return access.read("count");
    })) == 0);
    rejects([&] {
        (void) serial_actor.critical(actor + 1, 301, plain, allowed,
                [](State::Access& access) -> State::Value {
            access.write("count", std::int64_t{99});
            return std::int64_t{99};
        });
    });
    check(serial_actor.ready_to_suspend(actor + 1));
    // Explicit critical write remains legal on serial actors.
    check(std::get<std::int64_t>(serial_actor.critical(
            actor + 1, 302, checked, allowed,
            [](State::Access& access) -> State::Value {
        access.write("count", std::int64_t{1});
        return access.read("count");
    })) == 1);
    auto forged_write = checked;
    forged_write.callback_critical = false;
    rejects([&] {
        (void) state.critical(actor, 300, forged_write, allowed,
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
                // No nested read/write callback is legal, even when its
                // continuation is the same one already running this actor.
                rejects([&] {
                    (void) state.critical(actor, 100, checked, allowed,
                            [&](State::Access& inner) -> State::Value {
                        inner.write("count", std::int64_t{999});
                        return inner.read("count");
                    });
                });
                rejects([&] {
                    (void) state.critical(actor, 100, plain, readonly,
                            [&](State::Access& inner) -> State::Value {
                        return inner.read("count");
                    });
                });
                check(std::get<std::int64_t>(access.read("count")) == 1);
                check(!state.ready_to_suspend(actor));
            }
            return access.read("count");
        });
        check(std::get<std::int64_t>(next) == i + 1);
    }
    check(state.ready_to_suspend(actor));
    check(std::get<std::int64_t>(state.critical(actor, 201, checked, allowed,
            [&](State::Access& a) -> State::Value { return a.read("count"); })) == 100);

    // The runtime also checks each real operation against the compiler's
    // inferred effects, not just against the enclosing with declaration.
    auto inferred_write_only = checked;
    inferred_write_only.inferred = {
            static_cast<std::uint32_t>(FunctionEffect::StateWrite)};
    rejects([&] {
        (void) state.critical(actor, 208, inferred_write_only, allowed,
                [](State::Access& access) -> State::Value {
            return access.read("count");
        });
    });
    auto inferred_read_only = checked;
    inferred_read_only.inferred = readonly;
    rejects([&] {
        (void) state.critical(actor, 209, inferred_read_only, allowed,
                [](State::Access& access) -> State::Value {
            access.write("count", std::int64_t{999});
            return std::int64_t{999};
        });
    });
    check(state.ready_to_suspend(actor));

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
