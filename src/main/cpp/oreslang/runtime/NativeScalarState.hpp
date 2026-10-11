#pragma once

#include "oreslang/runtime/ActorArena.h"
#include "oreslang/runtime/CriticalGate.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Native executable substrate for compiler-lowered actor scalar state.
 *
 * The scheduler owns the CriticalGate::Token, which survives carrier migration
 * and may span awaits. A Scope represents a different, strictly lexical loan
 * to one physical ActorArena turn. The guest never receives a token, raw
 * OresArenaHandle, pointer or OresArenaTurn.
 *
 * LLVM source-level 'state { ... }' and 'handle_state' lowering is NOT wired
 * up yet; this class is the verified runtime contract it must target.
 */
class NativeScalarState final {
public:
    class Scope final {
    public:
        Scope(NativeScalarState& state, CriticalGate::Token ticket,
              OresArenaTurn physical_turn)
                : state_(&state), ticket_(ticket), turn_(physical_turn) {
            if (state_->retired_ || !state_->gate_.holds(ticket_)) {
                throw std::logic_error("actor StateAccess requires live logical critical ticket");
            }
            if (state_->active_ && state_->active_->ticket_ != ticket_) {
                throw std::logic_error("different continuation cannot nest actor StateAccess");
            }
            previous_ = state_->active_;
            state_->active_ = this;
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

        ~Scope() { close(); }

        std::int64_t read(std::size_t key) {
            check(key);
            std::int64_t result = 0;
            ores_actor_arena_read(state_->arena_, turn_, state_->slots_[key],
                                  0, &result, sizeof(result));
            return result;
        }

        void write(std::size_t key, std::int64_t value) {
            check(key);
            ores_actor_arena_write(state_->arena_, turn_, state_->slots_[key],
                                   0, &value, sizeof(value));
        }

        void close() noexcept {
            if (!live_) return;
            live_ = false;
            if (state_->active_ == this) {
                // Out-of-order outer closure must not resurrect an expired
                // ancestor when its nested view eventually unwinds.
                Scope* prior = previous_;
                while (prior && !prior->live_) prior = prior->previous_;
                state_->active_ = prior;
            }
        }

    private:
        void check(std::size_t key) const {
            if (!live_ || state_->retired_ || state_->active_ != this) {
                throw std::logic_error("actor StateAccess expired or fenced by nested borrow");
            }
            if (!state_->gate_.holds(ticket_)) {
                throw std::logic_error("actor StateAccess lost logical critical ownership");
            }
            if (key >= state_->slots_.size()) {
                throw std::out_of_range("unknown native scalar actor state key");
            }
        }

        NativeScalarState* state_;
        CriticalGate::Token ticket_;
        OresArenaTurn turn_;
        Scope* previous_ = nullptr;
        bool live_ = true;
    };

    NativeScalarState(CriticalGate& gate, OresActorArena* arena,
                      std::vector<OresArenaHandle> persistent_slots)
            : gate_(gate), arena_(arena), slots_(std::move(persistent_slots)) {
        if (!arena_ || slots_.empty()) {
            throw std::invalid_argument("native actor scalar state needs allocated slots");
        }
    }

    NativeScalarState(const NativeScalarState&) = delete;
    NativeScalarState& operator=(const NativeScalarState&) = delete;

    [[nodiscard]] Scope borrow(CriticalGate::Token ticket, OresArenaTurn turn) {
        return Scope(*this, ticket, turn);
    }

    /** Required before any scheduler yield, await, carrier handoff, or FFI. */
    void assert_can_suspend() const {
        if (active_) {
            throw std::logic_error("cannot suspend with a live actor StateAccess loan");
        }
    }

    /** Cancellation/teardown revokes further access even if token still held. */
    void retire() noexcept { retired_ = true; }

    [[nodiscard]] bool retired() const noexcept { return retired_; }
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

private:
    CriticalGate& gate_;
    OresActorArena* arena_;
    std::vector<OresArenaHandle> slots_;
    Scope* active_ = nullptr;
    bool retired_ = false;
};

} // namespace oreslang::runtime
