#include "oreslang/types/OwnershipChecker.hpp"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang {
namespace {

using Mode = Parameter::Mode;
using Functions = std::unordered_map<std::string, const Function*>;

struct LocalOwnership {
    Mode mode = Mode::Value;
    bool moved = false;
    std::size_t shared_loans = 0;
    bool exclusive_loan = false;
    std::string class_name;
    std::string field_name;
};
using Locals = std::unordered_map<std::string, LocalOwnership>;

[[noreturn]] void reject(const Expression& expr, const std::string& reason) {
    throw std::runtime_error(reason + " at byte " + std::to_string(expr.offset));
}

LocalOwnership& local(const Expression& expr, Locals& locals) {
    auto it = locals.find(expr.name);
    if (it == locals.end())
        reject(expr, "internal ownership error: undeclared parameter '" + expr.name + "'");
    return it->second;
}

void check_value(const Expression& expr, Locals& locals, const Functions& functions);

LocalOwnership& checked_field(const std::string& receiver, const std::string& field,
                               std::size_t offset, Locals& locals) {
    auto found = locals.find(receiver);
    if (found == locals.end() || found->second.class_name.empty() ||
            found->second.field_name != field)
        throw std::runtime_error("unknown class field or receiver at byte " + std::to_string(offset));
    if (found->second.moved)
        throw std::runtime_error("ownership violation: use-after-move class field at byte " +
                                 std::to_string(offset));
    return found->second;
}

struct Loan {
    std::string name;
    bool exclusive = false;
};

// Only named, OWNED int parameter storage is currently addressable. A loan
// exists only as a call argument and must be returned at the call boundary.
// No borrow of a temporary, return of a reference, or reborrow of an existing
// borrowed parameter can enter native LLVM lowering.
Loan acquire(const Expression& expr, Mode expected, Locals& locals) {
    const bool exclusive = expr.kind == Expression::Kind::BorrowMut;
    if (expr.kind != Expression::Kind::Borrow &&
        expr.kind != Expression::Kind::BorrowMut)
        reject(expr, "borrowed argument requires explicit rt borrow [mut]");
    if ((expected == Mode::BorrowMut) != exclusive)
        reject(expr, "borrow mode does not match borrowed parameter");
    if (!expr.left || expr.left->kind != Expression::Kind::Variable)
        reject(expr, "native borrow requires an addressable named int parameter");
    LocalOwnership& source = local(*expr.left, locals);
    // A borrowed parameter has a caller-held lifetime and already carries
    // provenance to addressable i64 storage. A nested call can temporarily
    // reborrow that same storage, but it cannot strengthen a shared borrow
    // into a mutable one. Its new loan ends at that nested call's return.
    if (source.mode == Mode::Borrow && exclusive)
        reject(expr, "cannot mutably reborrow shared parameter '" + expr.left->name + "'");
    if (source.moved)
        reject(expr, "borrow of explicitly moved value '" + expr.left->name + "'");
    if (source.exclusive_loan || (exclusive && source.shared_loans != 0))
        reject(expr, "conflicting active native borrow of '" + expr.left->name + "'");
    if (exclusive) source.exclusive_loan = true;
    else ++source.shared_loans;
    return {expr.left->name, exclusive};
}

void release(const Loan& loan, Locals& locals) {
    auto& source = locals.at(loan.name);
    if (loan.exclusive) source.exclusive_loan = false;
    else --source.shared_loans;
}

void check_call(const Expression& expr, Locals& locals, const Functions& functions) {
    const auto found = functions.find(expr.name);
    if (found == functions.end())
        reject(expr, "internal ownership error: unresolved function");
    const auto& parameters = found->second->parameters;
    if (parameters.size() != expr.arguments.size())
        reject(expr, "internal ownership error: call arity mismatch");

    // Preserve the full set of argument loans until the call has completed.
    // Nested calls observe outstanding outer-call loans. Evaluation is
    // strictly left-to-right, matching the LLVM emitter.
    std::vector<Loan> loans;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const Expression& argument = *expr.arguments[i];
        if (parameters[i].mode == Mode::Value) {
            if (!parameters[i].class_name.empty())
                reject(argument, "ownership violation: by-value class parameters require move/drop lowering");
            check_value(argument, locals, functions);
        } else {
            if (!argument.left || argument.left->kind != Expression::Kind::Variable)
                reject(argument, "native loan argument must name an addressable binding");
            const auto& source = local(*argument.left, locals);
            if (source.class_name != parameters[i].class_name)
                reject(argument, "ownership violation: borrowed argument type mismatch");
            loans.push_back(acquire(argument, parameters[i].mode, locals));
        }
    }
    for (auto it = loans.rbegin(); it != loans.rend(); ++it)
        release(*it, locals);
}

void check_value(const Expression& expr, Locals& locals, const Functions& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return;
        case Expression::Kind::Variable: {
            LocalOwnership& source = local(expr, locals);
            if (source.moved)
                reject(expr, "use of explicitly moved int '" + expr.name + "'");
            if (source.exclusive_loan)
                reject(expr, "read conflicts with active mutable borrow of '" + expr.name + "'");
            if (!source.class_name.empty())
                reject(expr, "ownership violation: class is non-Copy; use an explicit borrow");
            return;
        }
        case Expression::Kind::Field: {
            const auto& source = checked_field(expr.left->name, expr.name, expr.offset, locals);
            if (source.exclusive_loan)
                reject(expr, "ownership violation: field read conflicts with active mutable borrow");
            return;
        }
        case Expression::Kind::Copy:
        case Expression::Kind::Share:
        case Expression::Kind::Negate:
            check_value(*expr.left, locals, functions);
            return;
        case Expression::Kind::Take: {
            check_value(*expr.left, locals, functions);
            if (expr.left->kind == Expression::Kind::Variable) {
                LocalOwnership& source = local(*expr.left, locals);
                if (source.mode != Mode::Value)
                    reject(expr, "cannot rt take through a borrowed parameter");
                if (source.exclusive_loan || source.shared_loans != 0)
                    reject(expr, "cannot move int while borrow is active");
                source.moved = true;
            }
            return;
        }
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
            check_value(*expr.left, locals, functions);
            check_value(*expr.right, locals, functions);
            return;
        case Expression::Kind::Call:
            if (functions.at(expr.name)->returns_void)
                reject(expr, "void call cannot be used as an int expression");
            check_call(expr, locals, functions);
            return;
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut:
            reject(expr, "borrowed reference may only be passed to a borrowed parameter");
    }
    reject(expr, "invalid native ownership expression kind");
}

} // namespace

void validate_i64_ownership(const Program& program) {
    Functions functions;
    for (const auto& function : program.functions)
        functions.emplace(function.name, &function);
    std::unordered_map<std::string, std::string> class_fields;
    for (const auto& cls : program.classes)
        class_fields.emplace(cls.name, cls.field);
    for (const auto& function : program.functions) {
        Locals locals;
        for (const auto& param : function.parameters) {
            if (!param.class_name.empty()) {
                if (!class_fields.count(param.class_name))
                    throw std::runtime_error("unknown parameter class '" + param.class_name + "'");
                if (param.mode == Mode::Value)
                    throw std::runtime_error("ownership violation: by-value class parameter '" +
                        param.name + "' requires exclusive borrow or proven move/drop lowering");
            }
            LocalOwnership entry;
            entry.mode = param.mode;
            entry.class_name = param.class_name;
            if (!param.class_name.empty()) entry.field_name = class_fields.at(param.class_name);
            locals.emplace(param.name, std::move(entry));
        }
        for (const auto& binding : function.locals) {
            if (!binding.new_class)
                check_value(*binding.initializer, locals, functions);
            LocalOwnership entry;
            entry.class_name = binding.class_name;
            if (binding.new_class)
                entry.field_name = class_fields.at(binding.class_name);
            locals.emplace(binding.name, std::move(entry));
        }
        for (const auto& stmt : function.statements) {
            if (stmt.kind == Statement::Kind::FieldWrite) {
                auto& recv = checked_field(stmt.receiver, stmt.field, stmt.offset, locals);
                if (recv.mode == Mode::Borrow)
                    throw std::runtime_error("ownership violation: cannot mutate through shared class borrow");
                if (recv.exclusive_loan || recv.shared_loans != 0)
                    throw std::runtime_error("ownership violation: class field write conflicts with active borrow");
                check_value(*stmt.expression, locals, functions);
            } else {
                if (stmt.expression->kind != Expression::Kind::Call)
                    throw std::runtime_error("invalid native call statement");
                check_call(*stmt.expression, locals, functions);
            }
        }
        if (function.result) check_value(*function.result, locals, functions);
    }
}

} // namespace oreslang
