#include "oreslang/runtime/NativeSelectArbiter.hpp"
#include "oreslang/runtime/TaskCancellation.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <thread>
#include <vector>

using oreslang::runtime::NativeSelectArbiter;
using oreslang::runtime::TaskCancellation;

static void first_winner_revokes_all_armed_losers_once() {
    NativeSelectArbiter<3> select;
    std::array<int, 3> revoked{0, 0, 0};

    auto a = select.register_case(0);
    auto b = select.register_case(1);
    auto c = select.register_case(2);
    a.arm([&] { ++revoked[0]; });
    b.arm([&] { ++revoked[1]; });
    c.arm([&] { ++revoked[2]; });

    assert(b.try_win());
    assert(!a.try_win());
    assert(!c.try_win());
    assert(select.winner().has_value());
    assert(*select.winner() == 1);
    assert(!select.is_cancelled());
    assert(!a.is_winner());
    assert(b.is_winner());
    assert(!c.is_winner());
    assert(revoked[0] == 1);
    assert(revoked[1] == 0);
    assert(revoked[2] == 1);
    assert(select.unarmed_loser_count() == 0);
    assert(!select.cancel());
}

static void terminal_before_arm_revokes_loser_when_revoker_arrives() {
    NativeSelectArbiter<2> select;
    int winner_revoke = 0;
    int loser_revoke = 0;

    auto winner = select.register_case(0);
    auto loser = select.register_case(1);

    // Completion may race the producer that is still returning its external
    // cancellation handle. The arbiter must remember the loser until arm().
    assert(winner.try_win());
    assert(select.unarmed_loser_count() == 1);

    winner.arm([&] { ++winner_revoke; });
    loser.arm([&] { ++loser_revoke; });

    assert(winner_revoke == 0);
    assert(loser_revoke == 1);
    assert(select.unarmed_loser_count() == 0);
}

static void cancel_before_arm_revokes_late_registrations() {
    NativeSelectArbiter<2> select;
    int first_revoke = 0;
    int second_revoke = 0;

    auto first = select.register_case(0);
    auto second = select.register_case(1);
    first.arm([&] { ++first_revoke; });

    assert(select.cancel());
    assert(select.is_cancelled());
    assert(first_revoke == 1);
    assert(second_revoke == 0);
    assert(select.unarmed_loser_count() == 1);

    second.arm([&] { ++second_revoke; });
    assert(second_revoke == 1);
    assert(select.unarmed_loser_count() == 0);
    assert(!first.try_win());
    assert(!second.try_win());
    assert(!select.cancel());
}

static void task_revocation_cancels_select_without_reentrant_deadlock() {
    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    std::atomic<int> revoked{0};

    auto cancel = select.cancellation_handle();
    first.arm([&] {
        ++revoked;
        // Revocation callbacks run outside the select lock. Re-entering the
        // terminal operation must return false instead of deadlocking.
        assert(!cancel.cancel());
    });
    second.arm([&] {
        ++revoked;
        assert(!cancel.cancel());
    });

    auto task = TaskCancellation::root();
    auto subscription = task->on_revoke([cancel] {
        assert(cancel.cancel());
    });

    assert(task->cancel("select-owner-stop"));
    assert(select.is_cancelled());
    assert(revoked.load() == 2);
    assert(select.unarmed_loser_count() == 0);
    subscription.close();
    task->retire();
}

static void arbiter_destruction_revokes_live_registration() {
    int revoked = 0;
    NativeSelectArbiter<1>::Registration registration;
    {
        NativeSelectArbiter<1> select;
        registration = select.register_case(0);
        registration.arm([&] { ++revoked; });
    }
    assert(revoked == 1);
    assert(!registration.try_win());
}

static void registration_validation_fails_closed() {
    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);

    bool duplicate = false;
    try {
        (void)select.register_case(0);
    } catch (const std::logic_error&) {
        duplicate = true;
    }
    assert(duplicate);

    bool range = false;
    try {
        (void)select.register_case(2);
    } catch (const std::out_of_range&) {
        range = true;
    }
    assert(range);

    first.arm([] {});
    bool double_arm = false;
    try {
        first.arm([] {});
    } catch (const std::logic_error&) {
        double_arm = true;
    }
    assert(double_arm);

    assert(select.cancel());

    bool terminal_registration = false;
    try {
        (void)select.register_case(1);
    } catch (const std::logic_error&) {
        terminal_registration = true;
    }
    assert(terminal_registration);
}

static void conditional_commit_only_wins_after_operation_commits() {
    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    int first_revoke = 0;
    int second_revoke = 0;
    int commits = 0;

    first.arm([&] { ++first_revoke; });
    second.arm([&] { ++second_revoke; });

    assert(!first.try_win_if([&] {
        ++commits;
        return false;
    }));
    assert(commits == 1);
    assert(!select.winner().has_value());
    assert(first_revoke == 0);
    assert(second_revoke == 0);

    assert(second.try_win_if([&] {
        ++commits;
        return true;
    }));
    assert(commits == 2);
    assert(select.winner().has_value() && *select.winner() == 1);
    assert(first_revoke == 1);
    assert(second_revoke == 0);
}

static void throwing_commit_leaves_select_pending() {
    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    int first_revoke = 0;
    int second_revoke = 0;
    first.arm([&] { ++first_revoke; });
    second.arm([&] { ++second_revoke; });

    bool observed = false;
    try {
        (void)first.try_win_if([]() -> bool {
            throw std::runtime_error("commit failure");
        });
    } catch (const std::runtime_error&) {
        observed = true;
    }
    assert(observed);
    assert(!select.winner().has_value());
    assert(first_revoke == 0);
    assert(second_revoke == 0);

    assert(second.try_win());
    assert(first_revoke == 1);
    assert(second_revoke == 0);
}

static void commit_reentry_fails_closed_without_losing_registrations() {
    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    int first_revoked = 0;
    int second_revoked = 0;
    first.arm([&] { ++first_revoked; });
    second.arm([&] { ++second_revoked; });

    const auto rejected = [&](auto attempt) {
        bool threw = false;
        try {
            (void)first.try_win_if([&]() -> bool {
                attempt();
                return true;
            });
        } catch (const std::logic_error&) {
            threw = true;
        }
        assert(threw);
        assert(!select.winner().has_value());
        assert(!select.is_cancelled());
        assert(first_revoked == 0 && second_revoked == 0);
    };

    rejected([&] { (void)first.try_win(); });
    rejected([&] { (void)select.cancellation_handle().cancel(); });
    rejected([&] { (void)select.winner(); });
    rejected([&] { (void)first.is_winner(); });
    rejected([&] { first.arm([] {}); });
    rejected([&] { (void)select.register_case(0); });

    assert(second.try_win());
    assert(select.winner().has_value() && *select.winner() == 1);
    assert(first_revoked == 1 && second_revoked == 0);
}

static void nested_other_arbiter_is_allowed_but_cycle_is_rejected() {
    NativeSelectArbiter<2> outer;
    NativeSelectArbiter<2> inner;
    auto outer_case = outer.register_case(0);
    auto inner_case = inner.register_case(0);
    outer_case.arm([] {});
    inner_case.arm([] {});

    bool cycle_rejected = false;
    assert(outer_case.try_win_if([&] {
        // A -> B is permitted. B -> A must be rejected before locking A.
        assert(inner_case.try_win_if([&] {
            try {
                (void)outer.winner();
            } catch (const std::logic_error&) {
                cycle_rejected = true;
            }
            return true;
        }));
        return true;
    }));
    assert(cycle_rejected);
    assert(outer.winner().has_value() && *outer.winner() == 0);
    assert(inner.winner().has_value() && *inner.winner() == 0);
}

// Regression: a per-Capacity thread-local commit scope silently misses this
// cross-specialization cycle and deadlocks while trying to lock outer again.
static void mixed_capacity_commit_cycle_fails_closed() {
    NativeSelectArbiter<2> outer;
    NativeSelectArbiter<3> inner;
    auto first = outer.register_case(0);
    auto second = outer.register_case(1);
    auto nested = inner.register_case(0);
    int revoked = 0;
    first.arm([] {});
    second.arm([&] { ++revoked; });
    nested.arm([] {});

    bool cycle_rejected = false;
    assert(first.try_win_if([&] {
        assert(nested.try_win_if([&] {
            try {
                (void)second.is_winner();
            } catch (const std::logic_error&) {
                cycle_rejected = true;
            }
            return true;
        }));
        return true;
    }));
    assert(cycle_rejected);
    assert(outer.winner().has_value() && *outer.winner() == 0);
    assert(inner.winner().has_value() && *inner.winner() == 0);
    assert(revoked == 1);
}

static void throwing_revoker_does_not_skip_other_losers() {
    NativeSelectArbiter<3> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    auto third = select.register_case(2);
    int second_revoke = 0;
    int third_revoke = 0;

    first.arm([] {});
    second.arm([&] {
        ++second_revoke;
        throw std::runtime_error("revoker failure");
    });
    third.arm([&] { ++third_revoke; });

    assert(first.try_win());
    assert(second_revoke == 1);
    assert(third_revoke == 1);
    assert(select.winner().has_value() && *select.winner() == 0);
    assert(select.unarmed_loser_count() == 0);
    assert(select.has_cleanup_failure());

    bool observed = false;
    try {
        std::rethrow_exception(select.cleanup_failure());
    } catch (const std::runtime_error&) {
        observed = true;
    }
    assert(observed);
}

static void concurrent_ready_race_has_exactly_one_winner() {
    constexpr std::size_t count = 16;
    NativeSelectArbiter<count> select;
    std::array<NativeSelectArbiter<count>::Registration, count> registrations;
    std::array<std::atomic<int>, count> revoked{};
    for (auto& value : revoked) value.store(0);
    std::atomic<bool> start{false};
    std::atomic<int> winner_count{0};

    for (std::size_t i = 0; i < count; ++i) {
        registrations[i] = select.register_case(i);
        registrations[i].arm([&, i] { revoked[i].fetch_add(1); });
    }

    std::vector<std::thread> threads;
    threads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        threads.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (registrations[i].try_win())
                winner_count.fetch_add(1);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();

    assert(winner_count.load() == 1);
    assert(select.winner().has_value());
    const auto winning_index = *select.winner();

    int revoked_total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const int observed = revoked[i].load();
        if (i == winning_index) assert(observed == 0);
        else assert(observed == 1);
        revoked_total += observed;
    }
    assert(revoked_total == static_cast<int>(count - 1));
    assert(select.unarmed_loser_count() == 0);
}

int main() {
    first_winner_revokes_all_armed_losers_once();
    terminal_before_arm_revokes_loser_when_revoker_arrives();
    cancel_before_arm_revokes_late_registrations();
    task_revocation_cancels_select_without_reentrant_deadlock();
    arbiter_destruction_revokes_live_registration();
    registration_validation_fails_closed();
    conditional_commit_only_wins_after_operation_commits();
    throwing_commit_leaves_select_pending();
    commit_reentry_fails_closed_without_losing_registrations();
    nested_other_arbiter_is_allowed_but_cycle_is_rejected();
    mixed_capacity_commit_cycle_fails_closed();
    throwing_revoker_does_not_skip_other_losers();
    concurrent_ready_race_has_exactly_one_winner();
    return 0;
}
