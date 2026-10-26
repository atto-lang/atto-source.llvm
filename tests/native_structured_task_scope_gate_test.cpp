#include "oreslang/runtime/StructuredTaskScopeGate.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>

using oreslang::runtime::StructuredTaskScopeGate;

static std::exception_ptr failure(const char* message) {
    return std::make_exception_ptr(std::runtime_error(message));
}

static void terminal_alone_is_not_quiescence() {
    StructuredTaskScopeGate scope;
    auto child = scope.reserve();
    child->began_execution();
    child->attach_completion(11);
    assert(!scope.close());

    child->terminal_ready(11);
    assert(!scope.drained());

    child->quiesced();
    assert(scope.drained());
    assert(!scope.failure());
}

static void quiescence_alone_is_not_terminal_authority() {
    StructuredTaskScopeGate scope;
    auto child = scope.reserve();
    child->began_execution();
    child->attach_completion(12);
    assert(!scope.close());

    child->quiesced();
    assert(!scope.drained());

    child->terminal_ready(12);
    assert(scope.drained());
}

static void witness_order_is_irrelevant_but_both_are_required() {
    for (std::uint64_t i = 1; i <= 512; ++i) {
        StructuredTaskScopeGate scope;
        auto child = scope.reserve();
        child->began_execution();
        child->attach_completion(1000 + i);
        scope.close();

        if ((i & 1U) == 0) {
            child->terminal_ready(1000 + i);
            assert(!scope.drained());
            child->quiesced();
        } else {
            child->quiesced();
            assert(!scope.drained());
            child->terminal_ready(1000 + i);
        }
        assert(scope.drained());
    }
}

static void failed_admission_is_the_only_no_completion_failure_path() {
    StructuredTaskScopeGate scope;
    auto child = scope.reserve();
    scope.close();

    child->failed_admission(failure("native admission failed"));
    assert(scope.drained());
    assert(scope.failure());

    bool rejected = false;
    try {
        child->attach_completion(99);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert(rejected);
}

static void exact_completion_identity_is_not_replaceable() {
    StructuredTaskScopeGate scope;
    auto child = scope.reserve();
    child->began_execution();
    child->attach_completion(21);

    bool mismatch = false;
    try {
        child->terminal_ready(22);
    } catch (const std::logic_error&) {
        mismatch = true;
    }
    assert(mismatch);

    child->terminal_ready(21);
    child->quiesced();
    assert(scope.close());
}

static void close_seals_roots_but_live_child_may_admit_descendants() {
    StructuredTaskScopeGate scope;
    auto parent = scope.reserve();
    parent->began_execution();
    parent->attach_completion(31);
    assert(!scope.close());

    bool root_rejected = false;
    try {
        (void)scope.reserve();
    } catch (const std::logic_error&) {
        root_rejected = true;
    }
    assert(root_rejected);

    auto child = scope.reserve(parent);
    child->began_execution();
    child->attach_completion(32);

    parent->terminal_ready(31);
    bool stale_parent_rejected = false;
    try {
        (void)scope.reserve(parent);
    } catch (const std::logic_error&) {
        stale_parent_rejected = true;
    }
    assert(stale_parent_rejected);

    parent->quiesced();
    assert(!scope.drained());

    child->terminal_ready(32);
    child->quiesced();
    assert(scope.drained());
}

static void cancellation_seals_descendants_before_physical_retirement() {
    StructuredTaskScopeGate scope;
    auto parent = scope.reserve();
    parent->began_execution();
    parent->attach_completion(41);
    auto child = scope.reserve(parent);
    child->began_execution();
    child->attach_completion(42);

    assert(!scope.cancel_and_drain("scope cancelled"));
    assert(scope.cancelled());
    assert(parent->cancellation_authority()->is_cancelled());
    assert(child->cancellation_authority()->is_cancelled());

    bool rejected = false;
    try {
        (void)scope.reserve(parent);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert(rejected);

    // Cancellation authority alone is not a physical retirement witness.
    parent->terminal_ready(41, failure("parent cancelled"));
    child->terminal_ready(42, failure("child cancelled"));
    assert(!scope.drained());

    parent->quiesced();
    assert(!scope.drained());
    child->quiesced();
    assert(scope.drained());
    assert(scope.failure());
}

static void late_close_hook_is_delivered_exactly_once() {
    StructuredTaskScopeGate scope;
    auto child = scope.reserve();
    std::atomic<int> close_calls{0};

    assert(!scope.close());
    child->on_scope_close([&] {
        ++close_calls;
        child->retire_unstarted();
    });

    assert(close_calls.load() == 1);
    assert(scope.drained());
    scope.close();
    assert(close_calls.load() == 1);
}

static void close_vs_descendant_admission_is_linearized() {
    for (int attempt = 0; attempt < 512; ++attempt) {
        StructuredTaskScopeGate scope;
        auto parent = scope.reserve();
        parent->began_execution();
        const std::uint64_t parent_id = 10000U + std::uint64_t(attempt);
        parent->attach_completion(parent_id);
        scope.close();

        std::atomic<bool> go{false};
        std::atomic<bool> admitted{false};
        StructuredTaskScopeGate::RegistrationPtr child;

        std::thread spawn([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            try {
                auto local = scope.reserve(parent);
                local->began_execution();
                local->attach_completion(parent_id + 100000U);
                child = local;
                admitted.store(true, std::memory_order_release);
            } catch (const std::logic_error&) {
            }
        });
        std::thread terminal([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            parent->terminal_ready(parent_id);
        });

        go.store(true, std::memory_order_release);
        spawn.join();
        terminal.join();

        parent->quiesced();
        if (admitted.load(std::memory_order_acquire)) {
            assert(child);
            assert(!scope.drained());
            child->terminal_ready(parent_id + 100000U);
            child->quiesced();
        }
        assert(scope.drained());

        bool rejected = false;
        try {
            (void)scope.reserve(parent);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        assert(rejected);
    }
}

static void cancellation_vs_admission_never_reopens_scope() {
    for (int attempt = 0; attempt < 256; ++attempt) {
        StructuredTaskScopeGate scope;
        auto parent = scope.reserve();
        parent->began_execution();
        const std::uint64_t parent_id = 20000U + std::uint64_t(attempt);
        parent->attach_completion(parent_id);

        std::atomic<bool> go{false};
        StructuredTaskScopeGate::RegistrationPtr child;

        std::thread spawn([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            try {
                auto local = scope.reserve(parent);
                local->began_execution();
                local->attach_completion(parent_id + 200000U);
                child = local;
            } catch (const std::logic_error&) {
            }
        });
        std::thread cancel([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            scope.cancel_and_drain("race cancel");
        });

        go.store(true, std::memory_order_release);
        spawn.join();
        cancel.join();

        bool rejected = false;
        try {
            (void)scope.reserve(parent);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        assert(rejected);

        parent->terminal_ready(parent_id, failure("cancelled"));
        parent->quiesced();
        if (child) {
            assert(child->cancellation_authority()->is_cancelled());
            child->terminal_ready(parent_id + 200000U, failure("cancelled child"));
            child->quiesced();
        }
        assert(scope.drained());
    }
}

int main() {
    terminal_alone_is_not_quiescence();
    quiescence_alone_is_not_terminal_authority();
    witness_order_is_irrelevant_but_both_are_required();
    failed_admission_is_the_only_no_completion_failure_path();
    exact_completion_identity_is_not_replaceable();
    close_seals_roots_but_live_child_may_admit_descendants();
    cancellation_seals_descendants_before_physical_retirement();
    late_close_hook_is_delivered_exactly_once();
    close_vs_descendant_admission_is_linearized();
    cancellation_vs_admission_never_reopens_scope();
}
