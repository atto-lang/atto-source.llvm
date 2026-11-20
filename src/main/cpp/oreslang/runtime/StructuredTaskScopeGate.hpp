#pragma once

#include "oreslang/runtime/TaskCancellation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/*
 * Runtime-private structured Task lifetime gate.
 *
 * This is not a source TaskScope ABI and does not enable source Task lowering.
 * It exists so the native scheduler has the same acceptance contract as the
 * Java/Graal runtime before source execution is wired:
 *
 *   logical terminal authority != physical quiescence
 *   close seals root admission but permits descendants of a live child
 *   cancellation seals every new descendant immediately
 *   failed admission is the only no-completion retirement path
 *   one exact completion identity owns terminal authority
 *
 * The owner MUST keep this scope alive until drained() is true.
 */
class StructuredTaskScopeGate final {
public:
    class Registration;
    using RegistrationPtr = std::shared_ptr<Registration>;

    StructuredTaskScopeGate()
        : cancellation_root_(TaskCancellation::root()) {}

    StructuredTaskScopeGate(const StructuredTaskScopeGate&) = delete;
    StructuredTaskScopeGate& operator=(const StructuredTaskScopeGate&) = delete;

    RegistrationPtr reserve(RegistrationPtr parent = {}) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (cancelled_)
            throw std::logic_error("Task spawned into cancelled structured scope");
        if (closing_ && (!parent || !parent->may_spawn_into(this)))
            throw std::logic_error(
                "Task spawned into closing structured scope without a live child permit");

        auto authority = cancellation_root_->child();
        auto registration = std::shared_ptr<Registration>(
            new Registration(this, std::move(authority)));
        children_.emplace(registration.get(), registration);
        ++active_;
        return registration;
    }

    bool close() {
        std::vector<RegistrationPtr> snapshot;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            closing_ = true;
            snapshot = snapshot_children_locked();
        }
        for (const auto& child : snapshot) child->request_scope_close();
        return drained();
    }

    bool cancel_and_drain(std::string reason = "structured scope cancelled") {
        if (reason.empty())
            throw std::invalid_argument("structured scope cancellation reason is empty");

        std::vector<RegistrationPtr> snapshot;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            closing_ = true;
            cancelled_ = true;
            if (!first_failure_) {
                first_failure_ =
                    std::make_exception_ptr(std::runtime_error(reason));
            }
            snapshot = snapshot_children_locked();
        }

        // Seal scope admission before executing any close/revocation hook.
        for (const auto& child : snapshot) child->request_scope_close();
        try {
            (void)cancellation_root_->cancel(std::move(reason));
        } catch (...) {
            record_failure(std::current_exception());
        }
        return drained();
    }

    bool drained() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return closing_ && active_ == 0;
    }

    bool closing() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return closing_;
    }

    bool cancelled() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return cancelled_;
    }

    std::size_t active_count() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return active_;
    }

    std::exception_ptr failure() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return first_failure_;
    }

    TaskCancellation::Ptr cancellation_root() const {
        return cancellation_root_;
    }

    class Registration final {
    public:
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

        TaskCancellation::Ptr cancellation_authority() const {
            return authority_;
        }

        void began_execution() {
            std::lock_guard<std::mutex> guard(mutex_);
            if (terminal_ || quiescent_ || notified_)
                throw std::logic_error(
                    "retired structured Task cannot begin execution");
            began_execution_ = true;
        }

        bool may_spawn() const {
            return may_spawn_into(scope_);
        }

        void attach_completion(std::uint64_t completion_identity) {
            if (completion_identity == 0)
                throw std::invalid_argument("completion identity must be nonzero");

            std::lock_guard<std::mutex> guard(mutex_);
            if (attached_identity_ != 0)
                throw std::logic_error(
                    "structured Task registration already attached");
            if (notified_) {
                if (terminal_identity_ == completion_identity) return;
                throw std::logic_error(
                    "retired structured Task cannot attach a different completion");
            }
            if (terminal_identity_ != 0 &&
                    terminal_identity_ != completion_identity)
                throw std::logic_error(
                    "structured Task terminal witness belongs to a different completion");
            attached_identity_ = completion_identity;
        }

        void terminal_ready(
                std::uint64_t completion_identity,
                std::exception_ptr failure = {}) {
            if (completion_identity == 0)
                throw std::invalid_argument("completion identity must be nonzero");

            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (notified_) {
                    if (terminal_identity_ == completion_identity) return;
                    throw std::logic_error(
                        "retired structured Task received a different completion");
                }
                if (attached_identity_ != 0 &&
                        attached_identity_ != completion_identity)
                    throw std::logic_error(
                        "structured Task terminal witness identity mismatch");
                if (terminal_identity_ != 0 &&
                        terminal_identity_ != completion_identity)
                    throw std::logic_error(
                        "structured Task terminal identity changed");
                terminal_identity_ = completion_identity;
                terminal_ = true;
                failure_ = std::move(failure);
            }
            notify_if_ready();
        }

        void quiesced() {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (notified_) return;
                quiescent_ = true;
            }
            notify_if_ready();
        }

        void failed_admission(std::exception_ptr failure) {
            if (!failure)
                throw std::invalid_argument("failed admission requires a failure");
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (notified_) return;
                if (attached_identity_ != 0 || terminal_identity_ != 0)
                    throw std::logic_error(
                        "attached structured Task cannot retire as failed admission");
                failure_ = std::move(failure);
                terminal_ = true;
                quiescent_ = true;
            }
            notify_if_ready();
        }

        void retire_unstarted() {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (notified_) return;
                if (began_execution_ || attached_identity_ != 0 ||
                        terminal_identity_ != 0)
                    throw std::logic_error(
                        "started structured Task cannot retire as unstarted");
                terminal_ = true;
                quiescent_ = true;
            }
            notify_if_ready();
        }

        void on_scope_close(std::function<void()> hook) {
            if (!hook)
                throw std::invalid_argument("scope close hook is empty");

            bool run_now = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (scope_close_hook_)
                    throw std::logic_error(
                        "structured Task already has a scope close hook");
                scope_close_hook_ = std::move(hook);
                if (scope_close_requested_ && !scope_close_hook_run_) {
                    scope_close_hook_run_ = true;
                    run_now = true;
                }
            }
            if (run_now) invoke_scope_close_hook();
        }

    private:
        friend class StructuredTaskScopeGate;

        Registration(
                StructuredTaskScopeGate* scope,
                TaskCancellation::Ptr authority)
            : scope_(scope),
              authority_(std::move(authority)) {}

        bool may_spawn_into(const StructuredTaskScopeGate* expected) const {
            std::lock_guard<std::mutex> guard(mutex_);
            return scope_ == expected &&
                   began_execution_ &&
                   !terminal_ &&
                   !quiescent_ &&
                   !notified_ &&
                   !authority_->is_cancelled();
        }

        void request_scope_close() {
            bool run = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                scope_close_requested_ = true;
                if (scope_close_hook_ && !scope_close_hook_run_) {
                    scope_close_hook_run_ = true;
                    run = true;
                }
            }
            if (run) invoke_scope_close_hook();
        }

        void invoke_scope_close_hook() noexcept {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                hook = scope_close_hook_;
            }
            try {
                if (hook) hook();
            } catch (...) {
                scope_->record_failure(std::current_exception());
            }
        }

        void notify_if_ready() {
            std::exception_ptr failure;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (notified_ || !terminal_ || !quiescent_) return;
                notified_ = true;
                failure = failure_;
            }

            // Native cancellation authority is retired only after the same
            // terminal + physical-quiescence conjunction has been proven.
            try {
                authority_->retire();
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
            scope_->child_done(this, std::move(failure));
        }

        StructuredTaskScopeGate* scope_;
        TaskCancellation::Ptr authority_;
        mutable std::mutex mutex_;
        bool began_execution_ = false;
        bool terminal_ = false;
        bool quiescent_ = false;
        bool notified_ = false;
        std::uint64_t attached_identity_ = 0;
        std::uint64_t terminal_identity_ = 0;
        std::exception_ptr failure_;
        std::function<void()> scope_close_hook_;
        bool scope_close_requested_ = false;
        bool scope_close_hook_run_ = false;
    };

private:
    std::vector<RegistrationPtr> snapshot_children_locked() const {
        std::vector<RegistrationPtr> result;
        result.reserve(children_.size());
        for (const auto& entry : children_) {
            if (auto child = entry.second.lock()) result.push_back(std::move(child));
        }
        return result;
    }

    void record_failure(std::exception_ptr failure) {
        if (!failure) return;
        std::lock_guard<std::mutex> guard(mutex_);
        if (!first_failure_) first_failure_ = std::move(failure);
    }

    void child_done(Registration* registration, std::exception_ptr failure) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = children_.find(registration);
        if (it == children_.end())
            throw std::logic_error(
                "structured Task registration retired twice");
        children_.erase(it);
        if (active_ == 0)
            throw std::logic_error("structured Task scope active underflow");
        --active_;
        if (failure && !first_failure_) first_failure_ = std::move(failure);
    }

    mutable std::mutex mutex_;
    std::unordered_map<Registration*, std::weak_ptr<Registration>> children_;
    std::size_t active_ = 0;
    bool closing_ = false;
    bool cancelled_ = false;
    std::exception_ptr first_failure_;
    TaskCancellation::Ptr cancellation_root_;
};

}  // namespace oreslang::runtime
