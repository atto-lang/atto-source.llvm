#include "oreslang/backend/SealedStateAdmission.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using oreslang::FunctionEffect;
using oreslang::FunctionEffects;

struct Expression {};
struct Field {
    enum class ScalarType { I64, Bool };
    std::string name;
    bool field_mutable = true;
    std::unique_ptr<Expression> initializer = std::make_unique<Expression>();
    ScalarType scalar_type = ScalarType::I64;
    ScalarType array_element_type = ScalarType::I64;
    std::string pending_type_alias;
    bool heap_owned = false, int_array = false, record_view = false;
    bool new_class = false, move_class = false, owner_mutable = false;
    std::string array_record_class, class_name, move_source, sum_type;
    std::vector<int> constructor_arguments;
};
struct Parameter {
    enum class ScalarType { I64, Bool, String };
    enum class Mode { Value, Borrow, BorrowMut };
    std::string name;
    std::string class_name, generic_type, pending_type_alias, sum_type;
    ScalarType scalar_type = ScalarType::I64;
    Mode mode = Mode::Value;
    Parameter(std::string n = {}) : name(std::move(n)) {}
};
struct Write {
    std::string field;
    std::unique_ptr<Expression> expression = std::make_unique<Expression>();
};
struct Loan {
    enum class Mode { Read, Write };
    Mode mode = Mode::Read;
    std::string binder = "s";
    std::vector<Write> writes;
    std::unique_ptr<Expression> result = std::make_unique<Expression>();
};
struct Method {
    enum class Return { I64, Bool };
    std::string name = "on_message";
    bool exported = true;
    bool returns_void = false;
    Return return_type = Return::I64;
    std::vector<Parameter> parameters{{"n"}};
    std::unique_ptr<Loan> actor_state_loan = std::make_unique<Loan>();
    bool has_declared_effects = true;
    FunctionEffects declared_effects{static_cast<std::uint32_t>(FunctionEffect::StateRead)};
    std::unique_ptr<Expression> result;
    std::vector<int> actor_field_writes, locals, actor_sends, actor_stops,
        actor_actions, statements, early_returns, actor_spawns;
    enum class AwaitForm { None, DirectHasFuture };
    AwaitForm ready_await_form = AwaitForm::None;
    std::vector<int> generic_parameters, caught_throw_defers;
    std::unique_ptr<Expression> scalar_while, caught_throw_value;
    std::string ready_await_actor, done_await_actor, ready_future_name,
        ready_future_actor, catch_binding, return_class, return_sum,
        pending_return_type_alias, method_owner;
    bool native_internal = false;
};
struct Actor {
    std::string name = "Counter";
    bool sealed_state = true;
    std::vector<Field> fields;
    Method on_message;
};

std::vector<Actor> valid() {
    std::vector<Actor> actors(1);
    actors.front().fields.emplace_back();
    actors.front().fields.front().name = "count";
    return actors;
}

std::size_t admitted=0, rejected=0;
void must_admit(std::vector<Actor> a) {
    oreslang::verify_sealed_actor_state_shape(a);
    ++admitted;
}
template<typename Modify>
void must_reject(Modify modify) {
    auto actors=valid();
    modify(actors.front());
    try { oreslang::verify_sealed_actor_state_shape(actors); }
    catch (const std::runtime_error&) { ++rejected; return; }
    throw std::runtime_error("invalid sealed-state AST was admitted");
}
} // namespace

int main() {
    must_admit(valid());
    auto empty=valid(); empty.front().fields.clear(); must_admit(std::move(empty));
    auto write=valid();
    auto &m=write.front().on_message;
    m.actor_state_loan->mode=Loan::Mode::Write;
    m.declared_effects.bits=static_cast<std::uint32_t>(FunctionEffect::StateWrite);
    m.actor_state_loan->writes.emplace_back();
    m.actor_state_loan->writes.front().field="count";
    must_admit(std::move(write));
    // Old scalar actors still compile through their existing handler lane.
    auto legacy=valid();
    legacy.front().sealed_state=false;
    legacy.front().on_message.actor_state_loan.reset();
    legacy.front().on_message.result=std::make_unique<Expression>();
    must_admit(std::move(legacy));
    // Confirm validation visits every actor, not just the first.
    auto mixed = valid();
    auto second = valid();
    second.front().name = "LegacyOther";
    second.front().sealed_state = false;
    second.front().on_message.actor_state_loan.reset();
    second.front().on_message.result = std::make_unique<Expression>();
    mixed.push_back(std::move(second.front()));
    must_admit(std::move(mixed));
    auto malformed_second = valid();
    auto malformed_actor = valid();
    malformed_actor.front().name = "OtherActor";
    malformed_actor.front().on_message.parameters.clear();
    malformed_second.push_back(std::move(malformed_actor.front()));
    bool rejected_second = false;
    try { oreslang::verify_sealed_actor_state_shape(malformed_second); }
    catch (const std::runtime_error&) { rejected_second = true; }
    if (!rejected_second)
        throw std::runtime_error("malformed second actor was admitted");
    ++rejected;

    // Immutable legacy scalar fields are permitted; malformed legacy ASTs
    // must be rejected before native codegen dereferences their storage.
    auto immutable_legacy = valid();
    immutable_legacy.front().sealed_state = false;
    immutable_legacy.front().fields.front().field_mutable = false;
    immutable_legacy.front().on_message.actor_state_loan.reset();
    immutable_legacy.front().on_message.result = std::make_unique<Expression>();
    must_admit(std::move(immutable_legacy));
    auto reject_legacy = [&](const std::function<void(Actor&)>& mutate) {
        auto actors = valid();
        auto& actor = actors.front();
        actor.sealed_state = false;
        actor.on_message.actor_state_loan.reset();
        actor.on_message.result = std::make_unique<Expression>();
        mutate(actor);
        try { oreslang::verify_sealed_actor_state_shape(actors); }
        catch (const std::runtime_error&) { ++rejected; return; }
        throw std::runtime_error("invalid legacy actor AST was admitted");
    };
    // Mutation regression: a forged sealed loan with an otherwise valid
    // legacy handler must still be rejected (no other missing data masks it).
    // Reject parser-bypassing legacy storage/parameter metadata previously ignored.
    reject_legacy([](Actor& a){a.fields.front().heap_owned=true;});
    reject_legacy([](Actor& a){a.fields.front().int_array=true;});
    reject_legacy([](Actor& a){a.fields.front().record_view=true;});
    reject_legacy([](Actor& a){a.fields.front().array_record_class="R";});
    reject_legacy([](Actor& a){a.fields.front().class_name="R";});
    reject_legacy([](Actor& a){a.fields.front().new_class=true;});
    reject_legacy([](Actor& a){a.fields.front().constructor_arguments.push_back(1);});
    reject_legacy([](Actor& a){a.fields.front().move_class=true;});
    reject_legacy([](Actor& a){a.fields.front().move_source="other";});
    reject_legacy([](Actor& a){a.fields.front().owner_mutable=true;});
    reject_legacy([](Actor& a){a.fields.front().sum_type="Option";});
    reject_legacy([](Actor& a){a.fields.front().pending_type_alias="Alias";});
    reject_legacy([](Actor& a){a.fields.front().array_element_type=Field::ScalarType::Bool;});
    reject_legacy([](Actor& a){a.on_message.parameters.front().scalar_type=Parameter::ScalarType::Bool;});
    reject_legacy([](Actor& a){a.on_message.parameters.front().mode=Parameter::Mode::BorrowMut;});
    reject_legacy([](Actor& a){a.on_message.parameters.front().class_name="R";});
    reject_legacy([](Actor& a){a.on_message.parameters.front().generic_type="T";});
    reject_legacy([](Actor& a){a.on_message.parameters.front().pending_type_alias="Alias";});
    reject_legacy([](Actor& a){a.on_message.parameters.front().sum_type="Option";});
    reject_legacy([](Actor& a){
        a.on_message.actor_state_loan=std::make_unique<Loan>();
    });
    reject_legacy([](Actor& a){a.fields.clear();});
    reject_legacy([](Actor& a){a.fields.front().initializer.reset();});
    reject_legacy([](Actor& a){a.fields.front().name.clear();});
    reject_legacy([](Actor& a){a.fields.front().name="state";});
    reject_legacy([](Actor& a){a.fields.front().name="n";});
    reject_legacy([](Actor& a){a.fields.emplace_back();a.fields.back().name="count";});
    reject_legacy([](Actor& a){a.fields.front().scalar_type=Field::ScalarType::Bool;});
    reject_legacy([](Actor& a){a.on_message.parameters.front().name.clear();});
    reject_legacy([](Actor& a){a.on_message.name="wrong";});
    reject_legacy([](Actor& a){a.on_message.exported=false;});
    reject_legacy([](Actor& a){a.on_message.return_type=Method::Return::Bool;});
    reject_legacy([](Actor& a){for(int i=0;i<16;++i){a.fields.emplace_back();a.fields.back().name="f"+std::to_string(i);}});

    must_reject([](Actor& a){a.sealed_state=false;});
    must_reject([](Actor& a){a.sealed_state=false;
        a.on_message.actor_state_loan.reset();});
    must_reject([](Actor& a){a.sealed_state=false;
        a.on_message.actor_state_loan.reset();
        a.on_message.result=std::make_unique<Expression>();
        a.on_message.parameters.clear();});
    must_reject([](Actor& a){a.sealed_state=false;
        a.on_message.actor_state_loan.reset();
        a.on_message.result=std::make_unique<Expression>();
        a.on_message.returns_void=true;});

    // Parser-bypassing ASTs cannot smuggle other storage/lifecycle lanes.
    must_reject([](Actor& a){a.fields.front().scalar_type=Field::ScalarType::Bool;});
    must_reject([](Actor& a){a.fields.front().heap_owned=true;});
    must_reject([](Actor& a){a.fields.front().int_array=true;});
    must_reject([](Actor& a){a.fields.front().record_view=true;});
    must_reject([](Actor& a){a.fields.front().array_record_class="R";});
    must_reject([](Actor& a){a.fields.front().class_name="R";});
    must_reject([](Actor& a){a.fields.front().new_class=true;});
    must_reject([](Actor& a){a.fields.front().constructor_arguments.push_back(1);});
    must_reject([](Actor& a){a.fields.front().move_class=true;});
    must_reject([](Actor& a){a.fields.front().move_source="other";});
    must_reject([](Actor& a){a.fields.front().owner_mutable=true;});
    must_reject([](Actor& a){a.fields.front().sum_type="Option";});
    must_reject([](Actor& a){a.on_message.generic_parameters.push_back(1);});
    must_reject([](Actor& a){a.on_message.scalar_while=std::make_unique<Expression>();});
    must_reject([](Actor& a){a.on_message.ready_await_actor="worker";});
    must_reject([](Actor& a){a.on_message.done_await_actor="worker";});
    must_reject([](Actor& a){a.on_message.ready_await_form=Method::AwaitForm::DirectHasFuture;});
    must_reject([](Actor& a){a.on_message.ready_future_name="f";});
    must_reject([](Actor& a){a.on_message.ready_future_actor="w";});
    must_reject([](Actor& a){a.on_message.caught_throw_value=std::make_unique<Expression>();});
    must_reject([](Actor& a){a.on_message.caught_throw_defers.push_back(1);});
    must_reject([](Actor& a){a.on_message.catch_binding="err";});
    must_reject([](Actor& a){a.on_message.return_class="R";});
    must_reject([](Actor& a){a.on_message.return_sum="Option";});
    must_reject([](Actor& a){a.on_message.pending_return_type_alias="Alias";});
    must_reject([](Actor& a){a.on_message.method_owner="owner";});
    must_reject([](Actor& a){a.on_message.native_internal=true;});

    // Sealed actors also require an unaliased, by-value i64 message ABI.
    must_reject([](Actor& a){a.fields.front().pending_type_alias="Alias";});
    must_reject([](Actor& a){a.fields.front().array_element_type=Field::ScalarType::Bool;});
    must_reject([](Actor& a){a.on_message.parameters.front().scalar_type=Parameter::ScalarType::Bool;});
    must_reject([](Actor& a){a.on_message.parameters.front().mode=Parameter::Mode::Borrow;});
    must_reject([](Actor& a){a.on_message.parameters.front().class_name="R";});
    must_reject([](Actor& a){a.on_message.parameters.front().generic_type="T";});
    must_reject([](Actor& a){a.on_message.parameters.front().pending_type_alias="Alias";});
    must_reject([](Actor& a){a.on_message.parameters.front().sum_type="Option";});
    must_reject([](Actor& a){a.on_message.parameters.clear();});
    must_reject([](Actor& a){a.on_message.parameters.emplace_back();});
    must_reject([](Actor& a){a.on_message.parameters.front().name.clear();});
    must_reject([](Actor& a){a.on_message.name="foo";});
    must_reject([](Actor& a){a.on_message.exported=false;});
    must_reject([](Actor& a){a.on_message.returns_void=true;});
    must_reject([](Actor& a){a.on_message.return_type=Method::Return::Bool;});
    must_reject([](Actor& a){a.fields.front().name="";});
    must_reject([](Actor& a){a.fields.front().name="state";});
    must_reject([](Actor& a){a.fields.front().name="n";});
    must_reject([](Actor& a){a.fields.emplace_back();a.fields.back().name="count";});
    must_reject([](Actor& a){a.fields.front().initializer.reset();});
    must_reject([](Actor& a){a.fields.front().field_mutable=false;});
    must_reject([](Actor& a){a.on_message.actor_state_loan.reset();});
    must_reject([](Actor& a){a.on_message.actor_state_loan->binder.clear();});
    must_reject([](Actor& a){a.on_message.actor_state_loan->binder="n";});
    must_reject([](Actor& a){a.on_message.actor_state_loan->result.reset();});
    must_reject([](Actor& a){a.on_message.has_declared_effects=false;});
    must_reject([](Actor& a){a.on_message.declared_effects.bits|=FunctionEffects::UnresolvedCallBit;});
    must_reject([](Actor& a){a.on_message.actor_state_loan->mode=static_cast<Loan::Mode>(255);});
    // A forged mode must fail even if state.write authority is declared:
    // otherwise it could be silently treated as Mode::Write.
    must_reject([](Actor& a){
        a.on_message.actor_state_loan->mode=static_cast<Loan::Mode>(255);
        a.on_message.declared_effects.bits=
            static_cast<std::uint32_t>(FunctionEffect::StateWrite);
    });
    must_reject([](Actor& a){a.on_message.declared_effects.bits=0;});
    must_reject([](Actor& a){a.on_message.actor_state_loan->writes.emplace_back();});
    must_reject([](Actor& a){a.on_message.actor_state_loan->mode=Loan::Mode::Write;});
    must_reject([](Actor& a){a.on_message.actor_field_writes.push_back(1);});
    must_reject([](Actor& a){a.on_message.locals.push_back(1);});
    must_reject([](Actor& a){a.on_message.actor_sends.push_back(1);});
    must_reject([](Actor& a){a.on_message.actor_stops.push_back(1);});
    must_reject([](Actor& a){a.on_message.actor_actions.push_back(1);});
    must_reject([](Actor& a){a.on_message.statements.push_back(1);});
    must_reject([](Actor& a){a.on_message.early_returns.push_back(1);});
    must_reject([](Actor& a){a.on_message.actor_spawns.push_back(1);});
    must_reject([](Actor& a){a.on_message.result=std::make_unique<Expression>();});
    must_reject([](Actor& a){a.on_message.actor_state_loan->mode=Loan::Mode::Write;
        a.on_message.declared_effects.bits=static_cast<std::uint32_t>(FunctionEffect::StateWrite);
        a.on_message.actor_state_loan->writes.emplace_back();
        a.on_message.actor_state_loan->writes.back().field="unknown";});
    must_reject([](Actor& a){a.on_message.actor_state_loan->mode=Loan::Mode::Write;
        a.on_message.declared_effects.bits=static_cast<std::uint32_t>(FunctionEffect::StateWrite);
        a.on_message.actor_state_loan->writes.emplace_back();
        a.on_message.actor_state_loan->writes.back().field="count";
        a.on_message.actor_state_loan->writes.back().expression.reset();});
    must_reject([](Actor& a){a.fields.clear();
        for (int i=0;i<17;i++) {a.fields.emplace_back();a.fields.back().name="f"+std::to_string(i);}});
    if (admitted != 6 || rejected != 108)
        throw std::runtime_error("sealed-state admission case count changed");
    std::cout << "Sealed-state admission: " << admitted << " valid / "
              << rejected << " rejected: PASS\n";
}
