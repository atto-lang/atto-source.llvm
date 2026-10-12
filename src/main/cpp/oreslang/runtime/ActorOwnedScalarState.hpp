#pragma once

#include "oreslang/runtime/CriticalStateLoan.hpp"

#include <cstdint>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace oreslang {

/**
 * Per-actor owned finite typed scalar state, backed by the existing native
 * generation-fenced loan scope. This is a concrete native runtime substrate,
 * not yet LLVM source AST/codegen for state {} and handle_state.
 *
 * The caller supplies scheduler-verified actor/continuation IDs. No mutex or
 * actor carrier is held by this class across await; only synchronous callbacks
 * are admitted. Effect metadata must be verified by the compiler.
 */
class ActorOwnedScalarState final {
public:
    static constexpr std::size_t kMaxFields = 256;
    static constexpr std::size_t kMaxStringBytes = 64 * 1024;
    static constexpr std::size_t kMaxTotalStringBytes = 1024 * 1024;
    using Value = std::variant<std::int64_t, bool, std::string>;
    enum class Type : std::uint8_t { Int, Bool, String };
    struct Field {
        Type type;
        Value initial;
    };

    explicit ActorOwnedScalarState(std::uint64_t actor_id,
                                   const std::map<std::string, Field>& schema,
                                   bool verified_serial_actor = false)
            : actor_id_(actor_id), verified_serial_actor_(verified_serial_actor) {
        if (actor_id == 0 || schema.empty() || schema.size() > kMaxFields) {
            throw std::invalid_argument("actor state requires owner and typed fields");
        }
        scope_.owner_actor_id = actor_id;
        for (const auto& [name, field] : schema) {
            if (name.empty() || !fits(field.type, field.initial)) {
                throw std::invalid_argument("actor state initializer does not match field type");
            }
            const auto next_bytes = string_bytes(field.initial);
            if (next_bytes > kMaxTotalStringBytes - total_string_bytes_) {
                throw std::invalid_argument("actor state exceeds aggregate string byte budget");
            }
            if (!fields_.emplace(name, Field{field.type, field.initial}).second) {
                throw std::invalid_argument("duplicate actor state key");
            }
            total_string_bytes_ += next_bytes;
        }
    }

    ActorOwnedScalarState(const ActorOwnedScalarState&) = delete;
    ActorOwnedScalarState& operator=(const ActorOwnedScalarState&) = delete;

    class Access final {
    public:
        Access(const Access&) = delete;
        Access& operator=(const Access&) = delete;
        Value read(const std::string& name) const {
            verify();
            return store_->at(name).initial;
        }
        void write(const std::string& name, Value value) {
            verify();
            if (mode_ != CriticalStateMode::Write) {
                throw std::runtime_error("read-only actor state loan cannot write");
            }
            auto& slot = store_->at(name);
            if (!fits(slot.type, value)) {
                throw std::invalid_argument("actor state field type mismatch");
            }
            const auto old_size = string_bytes(slot.initial);
            const auto next_size = string_bytes(value);
            if (next_size > kMaxTotalStringBytes
                    - (owner_->total_string_bytes_ - old_size)) {
                throw std::invalid_argument("actor state aggregate string byte quota");
            }
            slot.initial = std::move(value);
            owner_->total_string_bytes_ =
                    owner_->total_string_bytes_ - old_size + next_size;
        }
    private:
        friend class ActorOwnedScalarState;
        Access(ActorOwnedScalarState& owner, CriticalStateLoan& loan,
               CriticalStateMode mode)
            : owner_(&owner), loan_(&loan), store_(&owner.fields_),
              mode_(mode) {}
        static bool fits(Type type, const Value& value) {
            switch (type) {
                case Type::Int: return std::holds_alternative<std::int64_t>(value);
                case Type::Bool: return std::holds_alternative<bool>(value);
                case Type::String: return std::holds_alternative<std::string>(value);
            }
            return false;
        }
        void verify() const {
            if (!owner_ || owner_->retired_ ||
                    loan_->check() != ORES_ACTOR_STATE_OK) {
                throw std::runtime_error("stale or fenced actor state access");
            }
        }
        ActorOwnedScalarState* owner_;
        CriticalStateLoan* loan_;
        std::map<std::string, Field>* store_;
        CriticalStateMode mode_;
    };

    template <class Callback>
    Value critical(std::uint64_t actor_id, std::uint64_t continuation_id,
                   CriticalStateCallback metadata, FunctionEffects with_effects,
                   Callback&& callback) {
        if (retired_ || actor_id != actor_id_) {
            throw std::runtime_error("stale or foreign actor state owner");
        }
        CriticalStateLoan loan;
        if (loan.begin(scope_, actor_id, continuation_id, metadata,
                with_effects, verified_serial_actor_)
                != CriticalStateAdmission::Accept) {
            throw std::runtime_error("critical actor state/effect admission denied");
        }
        Access access(*this, loan, metadata.state_mode);
        // Only a scalar Value is returnable: no borrowed loan can escape in
        // the result. Destruction of loan unwinds on all exceptional exits.
        Value result = callback(access);
        if (loan.end() != ORES_ACTOR_STATE_OK) {
            throw std::runtime_error("critical state scope ended out of order");
        }
        return result;
    }

    bool ready_to_suspend(std::uint64_t actor_id) const noexcept {
        return ores_actor_state_ready_to_suspend(&scope_, actor_id)
                == ORES_ACTOR_STATE_OK;
    }

    void retire(std::uint64_t confirmed_actor_id) {
        if (confirmed_actor_id != actor_id_ ||
                ores_actor_state_ready_to_suspend(&scope_, actor_id_)
                        != ORES_ACTOR_STATE_OK) {
            throw std::runtime_error("cannot retire foreign actor or live state loan");
        }
        retired_ = true;
        fields_.clear();
        total_string_bytes_ = 0;
    }

private:
    static std::size_t string_bytes(const Value& value) {
        if (!std::holds_alternative<std::string>(value)) return 0;
        const auto length = std::get<std::string>(value).size();
        if (length > kMaxStringBytes) {
            throw std::invalid_argument("actor state string exceeds per-field byte quota");
        }
        return length;
    }
    static bool fits(Type type, const Value& value) {
        return Access::fits(type, value);
    }
    std::uint64_t actor_id_;
    // Trusted compiler/runtime actor-declaration property, not an argument
    // the untrusted callback can set to skip critical admission.
    bool verified_serial_actor_ = false;
    ores_actor_state_scope scope_{};
    std::map<std::string, Field> fields_;
    std::size_t total_string_bytes_ = 0;
    bool retired_ = false;
};

} // namespace oreslang
