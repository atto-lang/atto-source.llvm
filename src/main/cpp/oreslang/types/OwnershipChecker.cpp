#include "oreslang/types/OwnershipChecker.hpp"
#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/types/OwnershipCfg.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang {

// Select a callable by name+explicit-arity before validating loan/type contracts.
namespace {

using Mode = Parameter::Mode;
using Functions = std::unordered_map<std::string, const Function*>;

struct LocalOwnership {
    Mode mode = Mode::Value;
    bool moved = false;
    bool heap_owned = false; // Experimental region slots cannot transfer ownership yet.
    bool int_array = false; // Non-copy lexical array; no borrows or escapes.
    bool mutable_owner = false; // lexical let mut or exclusive borrowed parameter
    std::size_t shared_loans = 0;
    bool exclusive_loan = false;
    std::string class_name;
    std::unordered_set<std::string> class_fields;
    std::unordered_set<std::string> class_array_fields;
    std::unordered_set<std::string> final_fields;
    std::unordered_set<std::string> public_fields;
    // Primitive i64 class fields can be moved and later restored by a write.
    std::unordered_set<std::string> moved_fields;
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
            found->second.class_fields.count(field) == 0)
        throw std::runtime_error("unknown class field or receiver at byte " + std::to_string(offset));
    // The native subset has no instance methods. All member projections
    // therefore originate outside the declaring class.
    if (!found->second.public_fields.count(field))
        throw std::runtime_error("ownership violation: private class field '" +
                                 found->second.class_name + "." + field +
                                 "' is not accessible at byte " + std::to_string(offset));
    if (found->second.moved)
        throw std::runtime_error("ownership violation: use-after-move class field at byte " +
                                 std::to_string(offset));
    return found->second;
}

// An array handle is never an i64 source value; resolve its owned place.
LocalOwnership& array_owner(const Expression& receiver, Locals& locals, bool write) {
    const Expression* root = &receiver;
    std::string field;
    if (root->kind == Expression::Kind::Field && root->left &&
            root->left->kind == Expression::Kind::Variable) {
        field = root->name;
        root = root->left.get();
    } else if (root->kind != Expression::Kind::Variable) {
        reject(receiver, "native Array<int> operation requires a named owned array place");
    }
    auto it = locals.find(root->name);
    if (it == locals.end())
        reject(receiver, "native Array<int> operation requires a live owner");
    if (it->second.moved)
        reject(receiver, "ownership violation: use-after-move Array<int> owner");
    auto& owner = it->second;
    const bool valid = field.empty() ? owner.int_array :
        !owner.class_name.empty() && owner.class_array_fields.count(field);
    if (!field.empty() && valid && !owner.public_fields.count(field))
        reject(receiver, "ownership violation: private class field is not accessible");
    if (!valid || (!field.empty() && owner.moved_fields.count(field))) {
        if (field.empty())
            reject(receiver, write
                ? "ownership violation: Array<int> mutation requires live let mut owner"
                : "native Array<int> operation requires a live array owner");
        reject(receiver, "native Array<int> operation requires an owned Array<int> place");
    }
    if (owner.mode != Mode::Value)
        reject(receiver, "ownership violation: Array<int> cannot be accessed through a borrowed class");
    if (owner.exclusive_loan || (write && owner.shared_loans))
        reject(receiver, "ownership violation: Array<int> conflicts with active class loan");
    if (write && !owner.mutable_owner)
        reject(receiver, "ownership violation: Array<int> mutation requires live let mut owner");
    return owner;
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
    if (source.int_array)
        reject(expr, "ownership violation: native Array<int> cannot be borrowed or escaped");
    // A borrowed parameter has a caller-held lifetime and already carries
    // provenance to addressable i64 storage. A nested call can temporarily
    // reborrow that same storage, but it cannot strengthen a shared borrow
    // into a mutable one. Its new loan ends at that nested call's return.
    if (source.heap_owned && exclusive)
        reject(expr, "ownership violation: native owned int mutable borrow needs handle-aware writeback");
    if (source.mode == Mode::Borrow && exclusive)
        reject(expr, "cannot mutably reborrow shared parameter '" + expr.left->name + "'");
    if (exclusive && source.mode == Mode::Value && !source.mutable_owner)
        reject(expr, "cannot mutably borrow immutable owner '" + expr.left->name + "'");
    if (source.moved)
        reject(expr, (source.class_name.empty()
            ? "borrow of explicitly moved int '" : "ownership violation: borrow of moved class '")
            + expr.left->name + "'");
    if (!source.class_name.empty() && !source.moved_fields.empty())
        reject(expr, "ownership violation: cannot borrow a partially moved class '" +
            expr.left->name + "'");
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
    const auto found = functions.find(native_function_slot(expr.name, expr.arguments.size()));
    if (found == functions.end())
        reject(expr, "internal ownership error: unresolved function");
    const auto& parameters = found->second->parameters;
    if (parameters.size() != expr.arguments.size())
        reject(expr, "internal ownership error: call arity mismatch");

    // Preserve the full set of argument loans until the call has completed.
    // Nested calls observe outstanding outer-call loans. Evaluation is
    // strictly left-to-right, matching the LLVM emitter.
    if (!expr.argument_names.empty() &&
            expr.argument_names.size() != expr.arguments.size())
        reject(expr, "invalid native named call argument layout");
    if (!expr.type_arguments_present && !expr.type_arguments.empty())
        reject(expr, "native call contains hidden generic type arguments");

    const auto& generics = found->second->generic_parameters;
    if (expr.type_arguments_present && generics.empty())
        reject(expr, "call-site type arguments require generic function");
    if (!generics.empty()) {
        if (expr.type_arguments.size() > generics.size())
            reject(expr, "too many generic type arguments");
        std::unordered_set<std::string> inferred;
        std::unordered_set<std::string> assigned;
        std::unordered_set<std::string> holes;
        std::size_t positional_type = 0;
        bool named_type_seen = false;
        for (const auto& arg : expr.type_arguments) {
            std::size_t slot = positional_type;
            if (!arg.name.empty()) {
                named_type_seen = true;
                auto it = std::find_if(generics.begin(), generics.end(),
                    [&](const GenericParameter& candidate) {
                        return candidate.name == arg.name;
                    });
                if (it == generics.end())
                    reject(expr, "unknown named generic argument '" + arg.name + "'");
                slot = static_cast<std::size_t>(it - generics.begin());
            } else {
                if (named_type_seen)
                    reject(expr, "positional generic argument after named argument");
                ++positional_type;
            }
            if (slot >= generics.size())
                reject(expr, "too many generic type arguments");
            const std::string& name = generics[slot].name;
            if (!assigned.insert(name).second)
                reject(expr, "duplicate generic argument '" + name + "'");
            if (arg.infer) {
                if (!arg.type.empty())
                    reject(expr, "generic inference hole contains explicit type");
                holes.insert(name);
            } else {
                if (arg.type != "int")
                    reject(expr, "unsupported native generic type specialization");
                inferred.insert(name);
            }
        }
        for (const auto& parameter : parameters)
            if (!parameter.generic_type.empty())
                inferred.insert(parameter.generic_type);
        for (const auto& slot : generics) {
            if (inferred.count(slot.name)) continue;
            if (!holes.count(slot.name) && !slot.default_type.empty() &&
                    (slot.default_type == "int" ||
                     inferred.count(slot.default_type))) {
                inferred.insert(slot.name);
                continue;
            }
            reject(expr, "cannot infer generic parameter '" + slot.name + "'");
        }
    }

    std::vector<Loan> loans;
    std::vector<bool> assigned(parameters.size(), false);
    std::size_t positional = 0;
    bool named_seen = false;
    for (std::size_t i = 0; i < expr.arguments.size(); ++i) {
        const Expression& argument = *expr.arguments[i];
        const std::string label = expr.argument_names.empty()
                ? std::string{} : expr.argument_names[i];
        std::size_t formal = positional;
        if (!label.empty()) {
            named_seen = true;
            formal = 0;
            while (formal < parameters.size() &&
                   parameters[formal].name != label) ++formal;
            if (formal == parameters.size())
                reject(argument, "ownership violation: unknown named parameter '" + label + "'");
        } else {
            if (named_seen)
                reject(argument, "ownership violation: positional argument after named argument");
            ++positional;
        }
        if (formal >= parameters.size() || assigned[formal])
            reject(argument, "ownership violation: duplicate named parameter binding");
        assigned[formal] = true;

        // Acquire loans in *lexical source order* while using the target
        // parameter's ownership mode. A later argument must observe earlier
        // mutable/shared loans even if its formal slot is numerically lower.
        const Parameter& parameter = parameters[formal];
        if (parameter.mode == Mode::Value) {
            if (!parameter.class_name.empty())
                reject(argument, "ownership violation: by-value class parameters require move/drop lowering");
            check_value(argument, locals, functions);
        } else {
            // Acquire first so malformed loans preserve precise diagnostics
            // for temporaries and missing explicit 'rt borrow' markers.
            Loan loan = acquire(argument, parameter.mode, locals);
            const auto& source = locals.at(loan.name);
            if (source.class_name != parameter.class_name)
                reject(argument, "ownership violation: borrowed argument type mismatch");
            loans.push_back(std::move(loan));
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
            if (source.int_array)
                reject(expr, "ownership violation: native Array<int> is non-Copy and cannot escape");
            if (!source.class_name.empty())
                reject(expr, "ownership violation: class is non-Copy; use an explicit borrow");
            return;
        }
        case Expression::Kind::Field: {
            if (expr.name == "size" && expr.left &&
                    expr.left->kind == Expression::Kind::Field) {
                array_owner(*expr.left, locals, false);
                return;
            }
            if (expr.left && expr.left->kind == Expression::Kind::Variable) {
                const auto found = locals.find(expr.left->name);
                if (found != locals.end() && found->second.int_array) {
                    if (found->second.moved)
                        reject(expr, "ownership violation: use-after-move Array<int> owner");
                    if (expr.name != "size")
                        reject(expr, "ownership violation: only Array<int>.size is readable");
                    return;
                }
            }
            if (!expr.left || expr.left->kind != Expression::Kind::Variable)
                reject(expr, "native nested field requires owned Array<int>.size");
            const auto& source = checked_field(expr.left->name, expr.name, expr.offset, locals);
            if (source.class_array_fields.count(expr.name))
                reject(expr, "ownership violation: Array<int> field is non-Copy and cannot escape");
            if (source.moved_fields.count(expr.name) != 0)
                reject(expr, "ownership violation: use-after-move class field '" +
                    expr.left->name + "." + expr.name + "'");
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
            // The bounded class subset has only trivially destructible i64
            // fields. Lowering a field take as a load is valid only because
            // checker state invalidates that place until an explicit write.
            check_value(*expr.left, locals, functions);
            if (expr.left->kind == Expression::Kind::Variable) {
                LocalOwnership& source = local(*expr.left, locals);
                if (source.mode != Mode::Value)
                    reject(expr, "cannot rt take through a borrowed parameter");
                // Experimental native owned-i64 values transfer the scalar,
                // drop their old heap slot immediately and disarm the
                // generated exit-time drop flag. The emitted native backend
                // must keep that conditional flag in sync with this move.
                if (source.exclusive_loan || source.shared_loans != 0)
                    reject(expr, "cannot move int while borrow is active");
                source.moved = true;
            } else if (expr.left->kind == Expression::Kind::Field) {
                const Expression& field = *expr.left;
                // Nested array.size and other projections are not direct
                // owned class field places for destructive rt take.
                if (!field.left || field.left->kind != Expression::Kind::Variable)
                    reject(expr, "native rt take requires a direct named class field");
                LocalOwnership& source = checked_field(
                    field.left->name, field.name, field.offset, locals);
                if (source.final_fields.count(field.name))
                    reject(expr, "ownership violation: cannot take final class field");
                if (source.mode != Mode::Value)
                    reject(expr, "ownership violation: cannot take a class field through a borrowed parameter");
                if (source.exclusive_loan || source.shared_loans != 0)
                    reject(expr, "ownership violation: cannot take a class field with active loans");
                source.moved_fields.insert(field.name);
            }
            return;
        }
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
            check_value(*expr.left, locals, functions);
            check_value(*expr.right, locals, functions);
            return;
        case Expression::Kind::ArrayCall: {
            if (!expr.left)
                reject(expr, "native Array<int> operation requires a named owner");
            array_owner(*expr.left, locals, false);
            if (expr.name != "get" || expr.arguments.size() != 1)
                reject(expr, "native Array<int> expression supports only get(index)");
            check_value(*expr.arguments[0], locals, functions);
            return;
        }
        case Expression::Kind::Call: {
            const auto found = functions.find(
                native_function_slot(expr.name, expr.arguments.size()));
            if (found == functions.end())
                reject(expr, "internal ownership error: unresolved function");
            if (found->second->returns_void)
                reject(expr, "void call cannot be used as an int expression");
            check_call(expr, locals, functions);
            return;
        }
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut:
            reject(expr, "borrowed reference may only be passed to a borrowed parameter");
    }
    reject(expr, "invalid native ownership expression kind");
}

// Guard conditions must not mutate ownership or execute calls with hidden
// effects. A taken return has its own ownership snapshot and cannot poison
// the distinct fallthrough path. This is *not* general branch-lifetime proof.
void check_pure_guard(const Expression& expr, Locals& locals, const Functions& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
        case Expression::Kind::Variable:
        case Expression::Kind::Field:
            check_value(expr, locals, functions);
            return;
        case Expression::Kind::Negate:
        case Expression::Kind::Copy:
        case Expression::Kind::Share:
            check_pure_guard(*expr.left, locals, functions);
            return;
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
            check_pure_guard(*expr.left, locals, functions);
            check_pure_guard(*expr.right, locals, functions);
            return;
        case Expression::Kind::Take:
        case Expression::Kind::Call:
        case Expression::Kind::Borrow:
        case Expression::Kind::BorrowMut:
        case Expression::Kind::ArrayCall:
            reject(expr, "native guarded condition must be pure and cannot move, call or borrow");
    }
    reject(expr, "unsupported guarded condition");
}

// Only primitive stack int reads are safe in the initial caught-throw CFG.
// A syntactically pure Array<int>.size or heap-owned int read still invokes
// native runtime functions and therefore lacks a nonthrowing proof.
void check_caught_throw_scalar_read(const Expression& expr, Locals& locals,
                                   const Functions& functions) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return;
        case Expression::Kind::Variable: {
            const auto& place = local(expr, locals);
            if (place.heap_owned || place.int_array || !place.class_name.empty())
                reject(expr, "native caught throw scalar read requires stack i64");
            check_value(expr, locals, functions);
            return;
        }
        case Expression::Kind::Field: {
            if (!expr.left || expr.left->kind != Expression::Kind::Variable)
                reject(expr, "native caught throw scalar read requires direct named field");
            const auto& place = local(*expr.left, locals);
            if (place.heap_owned || place.int_array ||
                    place.class_array_fields.count(expr.name))
                reject(expr, "native caught throw scalar read requires stack i64");
            check_value(expr, locals, functions);
            return;
        }
        case Expression::Kind::Negate:
            check_caught_throw_scalar_read(*expr.left, locals, functions);
            return;
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
            check_caught_throw_scalar_read(*expr.left, locals, functions);
            check_caught_throw_scalar_read(*expr.right, locals, functions);
            return;
        default:
            reject(expr, "native caught throw scalar read cannot call, move or borrow");
    }
}

// A public C++ LLVM emission entrypoint accepts caller-built/transformed
// Program objects as well as parser-produced ASTs. Ownership and codegen
// dereference structurally required unique_ptrs; an invalid AST must throw
// a checked compiler error, never segfault or silently elide side effects.
// Run this before ANY ownership analysis or LLVM IR construction.
void preflight_native_ast_shape(const Program& program) {
    const auto fail = [](const std::string& what, std::size_t offset) {
        throw std::runtime_error("malformed native ownership AST: " + what +
                                 " at byte " + std::to_string(offset));
    };
    std::function<void(const Expression*, unsigned)> expression;
    expression = [&](const Expression* expr, unsigned depth) {
        if (!expr) fail("missing expression", 0);
        if (depth >= 256) fail("expression nesting exceeds 256", expr->offset);
        using K = Expression::Kind;
        const bool call = expr->kind == K::Call;
        if (!call && (!expr->argument_names.empty() ||
                !expr->type_arguments.empty() || expr->type_arguments_present))
            fail("non-call expression contains unlowered call metadata", expr->offset);
        switch (expr->kind) {
            case K::Integer:
            case K::Variable:
                // No implicit effects may hide underneath a scalar leaf.
                if (expr->left || expr->right || !expr->arguments.empty())
                    fail("scalar leaf contains hidden operands", expr->offset);
                break;
            case K::Negate:
            case K::Copy:
            case K::Take:
            case K::Share:
            case K::Borrow:
            case K::BorrowMut:
            case K::Field:
                if (!expr->left) fail("missing unary or receiver operand", expr->offset);
                if (expr->right || !expr->arguments.empty())
                    fail("unary or field expression contains hidden operands", expr->offset);
                break;
            case K::ArrayCall:
                if (!expr->left) fail("missing unary or receiver operand", expr->offset);
                if (expr->right)
                    fail("array call contains hidden operand", expr->offset);
                break;
            case K::Add:
            case K::Subtract:
            case K::Multiply:
                if (!expr->left || !expr->right)
                    fail("missing binary operand", expr->offset);
                if (!expr->arguments.empty())
                    fail("binary expression contains hidden arguments", expr->offset);
                break;
            case K::Call:
                if (expr->left || expr->right)
                    fail("direct call contains hidden receiver or operand", expr->offset);
                break;
            default:
                fail("unknown expression kind", expr->offset);
        }
        if (expr->left) expression(expr->left.get(), depth + 1);
        if (expr->right) expression(expr->right.get(), depth + 1);
        for (const auto& arg : expr->arguments)
            expression(arg.get(), depth + 1);
    };
    const auto local = [&](const LocalBinding& binding) {
        const unsigned specialized =
            static_cast<unsigned>(binding.int_array) +
            static_cast<unsigned>(binding.new_class) +
            static_cast<unsigned>(binding.move_class);
        if (specialized > 1)
            fail("local selects multiple storage variants", binding.offset);
        if (binding.heap_owned && specialized != 0)
            fail("specialized local cannot also be heap-owned", binding.offset);
        const bool class_storage = binding.new_class || binding.move_class;
        if (class_storage != !binding.class_name.empty())
            fail("local class metadata does not match storage variant", binding.offset);
        if (!binding.int_array && !binding.new_class &&
                !binding.move_class && !binding.initializer)
            fail("local has no initializer", binding.offset);
        if (binding.initializer && specialized != 0)
            fail("specialized local contains hidden initializer", binding.offset);
        if (!binding.new_class && !binding.constructor_arguments.empty())
            fail("non-constructor local contains hidden constructor arguments", binding.offset);
        if (!binding.move_class && !binding.move_source.empty())
            fail("non-move local contains hidden move source", binding.offset);
        if (binding.move_class && binding.move_source.empty())
            fail("class move local has no source", binding.offset);
        if (binding.initializer) expression(binding.initializer.get(), 0);
        for (const auto& arg : binding.constructor_arguments)
            expression(arg.get(), 0);
    };
    std::function<void(const Statement&, unsigned)> statement;
    statement = [&](const Statement& stmt, unsigned depth) {
        if (depth >= 64) fail("statement nesting exceeds 64", stmt.offset);
        using S = Statement::Kind;
        switch (stmt.kind) {
            case S::Call:
            case S::FieldWrite:
            case S::Defer:
                if (!stmt.expression) fail("statement has no expression", stmt.offset);
                if (stmt.branch_local || stmt.array_commit_length ||
                        !stmt.array_commit_values.empty() ||
                        stmt.condition_lhs || stmt.condition_rhs ||
                        !stmt.then_statements.empty() || !stmt.else_statements.empty())
                    fail("statement contains fields incompatible with its kind", stmt.offset);
                break;
            case S::Conditional:
                if (!stmt.condition_lhs || !stmt.condition_rhs)
                    fail("conditional missing guard", stmt.offset);
                if (stmt.branch_local || stmt.expression || stmt.array_commit_length ||
                        !stmt.array_commit_values.empty())
                    fail("conditional contains fields incompatible with its kind", stmt.offset);
                if (stmt.then_statements.size() > 16 || stmt.else_statements.size() > 16)
                    fail("conditional arm count exceeds parser bound", stmt.offset);
                break;
            case S::ScopedLocal:
                if (!stmt.branch_local)
                    fail("scoped local has no binding", stmt.offset);
                if (stmt.expression || stmt.array_commit_length ||
                        !stmt.array_commit_values.empty() ||
                        stmt.condition_lhs || stmt.condition_rhs ||
                        !stmt.then_statements.empty() || !stmt.else_statements.empty())
                    fail("scoped local contains fields incompatible with its kind", stmt.offset);
                break;
            case S::ArrayCommitFilled:
                if (!stmt.array_commit_length)
                    fail("array commit has no length", stmt.offset);
                if (stmt.branch_local || stmt.expression ||
                        stmt.condition_lhs || stmt.condition_rhs ||
                        !stmt.then_statements.empty() || !stmt.else_statements.empty())
                    fail("array commit contains fields incompatible with its kind", stmt.offset);
                if (stmt.array_commit_values.size() != 4)
                    fail("array commit fill count must equal four", stmt.offset);
                break;
            default:
                fail("unknown statement kind", stmt.offset);
        }
        // String-only targets are also kind-specific and must not carry
        // source-impossible metadata into a variant that ignores them.
        if (stmt.kind != S::FieldWrite && stmt.kind != S::Defer &&
                !stmt.field.empty())
            fail("statement contains hidden field target", stmt.offset);
        if (stmt.kind != S::FieldWrite && stmt.kind != S::Defer &&
                stmt.kind != S::ArrayCommitFilled && !stmt.receiver.empty())
            fail("statement contains hidden receiver", stmt.offset);
        if (stmt.kind == S::Defer &&
                (stmt.receiver.empty() != stmt.field.empty()))
            fail("deferred field target is incomplete", stmt.offset);
        if (stmt.kind == S::Call && stmt.expression &&
                stmt.expression->kind != Expression::Kind::Call &&
                stmt.expression->kind != Expression::Kind::ArrayCall)
            fail("call statement has non-call expression", stmt.offset);
        if (stmt.branch_local) local(*stmt.branch_local);
        if (stmt.expression) expression(stmt.expression.get(), 0);
        if (stmt.condition_lhs) expression(stmt.condition_lhs.get(), 0);
        if (stmt.condition_rhs) expression(stmt.condition_rhs.get(), 0);
        if (stmt.array_commit_length)
            expression(stmt.array_commit_length.get(), 0);
        for (const auto& value : stmt.array_commit_values)
            expression(value.get(), 0);
        for (const auto& nested : stmt.then_statements)
            statement(nested, depth + 1);
        for (const auto& nested : stmt.else_statements)
            statement(nested, depth + 1);
    };
    const auto function = [&](const Function& fn, bool actor_handler) {
        if (fn.early_returns.size() > 8)
            fail("guarded return count exceeds parser bound", fn.offset);
        const auto conditional_count = static_cast<std::size_t>(std::count_if(
            fn.statements.begin(), fn.statements.end(), [](const Statement& stmt) {
                return stmt.kind == Statement::Kind::Conditional;
            }));
        if (conditional_count > 8)
            fail("conditional CFG count exceeds parser bound", fn.offset);
        // Parser-produced functions always have a terminal value exactly when
        // their declared result is non-void. Direct AST callers must not let
        // LLVM receive a null return operand or smuggle an ignored value/effect
        // through a void declaration.
        if (fn.returns_void) {
            if (fn.result)
                fail("void function contains return value", fn.offset);
            if (!fn.early_returns.empty())
                fail("void function contains guarded value return", fn.offset);
        } else if (!fn.result) {
            fail("non-void function has no final result", fn.offset);
        }
        if (!fn.caught_throw_value &&
                (!fn.catch_binding.empty() || !fn.caught_throw_defers.empty()))
            fail("catch metadata exists without caught throw", fn.offset);
        if (fn.caught_throw_value && fn.catch_binding.empty())
            fail("caught throw is missing catch binding", fn.offset);
        if (fn.caught_throw_value) {
            if (fn.returns_void)
                fail("caught throw requires non-void function", fn.offset);
            if (!fn.early_returns.empty() || !fn.actor_actions.empty() ||
                    !fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
                    !fn.actor_stops.empty())
                fail("caught throw contains incompatible control effects", fn.offset);
            if (fn.caught_throw_defers.size() > 8)
                fail("caught throw defer count exceeds parser bound", fn.offset);
            if (fn.statements.size() > 16)
                fail("caught throw finally statement count exceeds parser bound", fn.offset);
            for (const auto& cleanup : fn.caught_throw_defers)
                if (cleanup.kind != Statement::Kind::FieldWrite)
                    fail("caught throw scoped defer is not a field write", cleanup.offset);
            for (const auto& cleanup : fn.statements)
                if (cleanup.kind != Statement::Kind::Defer ||
                        cleanup.receiver.empty() || cleanup.field.empty())
                    fail("caught throw finally contains non-field cleanup", cleanup.offset);
        }
        if (!actor_handler && !fn.actor_field_writes.empty())
            fail("ordinary function contains actor field writes", fn.offset);
        if (!fn.has_declared_effects && fn.declared_effects.bits != 0)
            fail("function contains hidden effect bits", fn.offset);
        if (fn.has_declared_effects &&
                (fn.declared_effects.bits == 0 || !fn.declared_effects.valid()))
            fail("function has invalid declared effect set", fn.offset);

        for (const auto& binding : fn.locals) local(binding);
        for (const auto& stmt : fn.statements) statement(stmt, 0);
        for (const auto& stmt : fn.caught_throw_defers) statement(stmt, 0);
        for (const auto& guard : fn.early_returns) {
            if (!guard.lhs || !guard.rhs || !guard.result)
                fail("guarded return has missing expression", guard.offset);
            expression(guard.lhs.get(), 0);
            expression(guard.rhs.get(), 0);
            expression(guard.result.get(), 0);
        }
        for (const auto& send : fn.actor_sends)
            expression(send.payload.get(), 0);
        for (const auto& write : fn.actor_field_writes)
            expression(write.expression.get(), 0);

        // actor_actions is the sole source-order plan consumed by LLVM
        // emission. A transformed Program must not omit, duplicate, or
        // mis-index a send/stop and thereby silently change actor effects.
        std::vector<bool> send_seen(fn.actor_sends.size(), false);
        std::vector<bool> stop_seen(fn.actor_stops.size(), false);
        for (const auto& action : fn.actor_actions) {
            switch (action.kind) {
                case ActorAction::Kind::Send:
                    if (action.index >= send_seen.size())
                        fail("actor send action index is out of range", fn.offset);
                    if (send_seen[action.index])
                        fail("actor send action is duplicated", fn.offset);
                    send_seen[action.index] = true;
                    break;
                case ActorAction::Kind::Stop:
                    if (action.index >= stop_seen.size())
                        fail("actor stop action index is out of range", fn.offset);
                    if (stop_seen[action.index])
                        fail("actor stop action is duplicated", fn.offset);
                    stop_seen[action.index] = true;
                    break;
                default:
                    fail("unknown actor action kind", fn.offset);
            }
        }
        if (std::find(send_seen.begin(), send_seen.end(), false) != send_seen.end())
            fail("actor send is missing from source-order action plan", fn.offset);
        if (std::find(stop_seen.begin(), stop_seen.end(), false) != stop_seen.end())
            fail("actor stop is missing from source-order action plan", fn.offset);

        if (fn.caught_throw_value) expression(fn.caught_throw_value.get(), 0);
        if (fn.result) expression(fn.result.get(), 0);
    };
    for (const auto& cls : program.classes) {
        if (cls.fields.empty() || cls.fields.size() > 16)
            fail("class field count is outside parser bounds", 0);
        if (cls.constructor_parameters.size() > 16)
            fail("class constructor parameter count exceeds parser bound", 0);
        if (!cls.explicit_constructor && !cls.constructor_parameters.empty())
            fail("implicit class contains constructor parameters", 0);
        bool owns_array = false;
        for (const auto& field : cls.fields) {
            owns_array = owns_array || field.int_array;
            if (field.int_array) {
                if (!field.mutable_referent)
                    fail("Array<int> class field lacks mutable referent capability", 0);
                if (field.initializer_expression)
                    fail("Array<int> class field contains hidden scalar initializer", 0);
            } else {
                if (!field.initializer_expression)
                    fail("scalar class field is missing initializer", 0);
                expression(field.initializer_expression.get(), 0);
                if (field.initializer_expression->kind == Expression::Kind::Integer &&
                        field.initial != field.initializer_expression->integer)
                    fail("scalar class field literal cache disagrees with initializer", 0);
            }
        }
        if (cls.explicit_constructor && owns_array)
            fail("explicit scalar constructor contains owned Array<int> field", 0);
    }
    for (const auto& actor : program.actors) {
        if (actor.fields.empty() || actor.fields.size() > 16)
            fail("actor field count is outside parser bounds", actor.offset);
        for (const auto& field : actor.fields) local(field);
        function(actor.on_message, true);
    }
    for (const auto& fn : program.functions) function(fn, false);
}

} // namespace

void validate_i64_ownership(const Program& program) {
    preflight_native_ast_shape(program);
    // Both admitted native scalar continuation forms must visit the SAME
    // reusable MIR suspension boundary. One is a cooperative 'suspend fnc'
    // declaration, the other a fuel-bounded --experimental-rt-scalar-loop
    // function that lowers into a pinned frame via ScalarLoopLowering.
    // Today each grammar holds only one owned i64 and no lexical loans.
    // Future expansion must not bypass this full-place suspend fence.
    const auto verify_scalar_suspension = [](const std::string& variable) {
        ownership::Graph graph;
        graph.entry = {{variable, {ownership::kOwned, 0, false}}};
        graph.blocks = {{{
            {ownership::Operation::Kind::Read, variable},
            {ownership::Operation::Kind::Suspend, ""},
            {ownership::Operation::Kind::Read, variable}
        }, {}}};
        ownership::verify(graph);
    };
    for (const auto& loop : program.cooperative_loops)
        verify_scalar_suspension(loop.variable);
    for (const auto& function : program.functions)
        if (function.scalar_while)
            verify_scalar_suspension(function.scalar_while->counter);

    const auto require_identifier = [](std::string_view value, const char* role) {
        if (!valid_native_source_identifier(value))
            throw std::runtime_error(std::string("ownership violation: invalid native ") +
                                     role + " identifier '" + std::string(value) + "'");
    };
    if (!program.module.empty())
        require_identifier(program.module, "module");

    const auto reserved_runtime_symbol = [&](const std::string& name) {
        if (name.rfind("oreslang_int_array_", 0) == 0) return true;
        if (!program.module.empty()) return false;
        return name == "abort" || name == "__oreslang_guest_main" ||
            name.rfind("oreslang_owned_i64_region_", 0) == 0 ||
            name.rfind("ores_actor_i64_heap_", 0) == 0 ||
            name.rfind("oreslang_source_actor_", 0) == 0 ||
            name.rfind("oreslang_actor_", 0) == 0 ||
            name.rfind("oreslang_wasm_actor_", 0) == 0;
    };

    Functions functions;
    std::unordered_set<std::string> function_names;
    for (const auto& function : program.functions) {
        require_identifier(function.name, "function");
        if (reserved_runtime_symbol(function.name))
            throw std::runtime_error(
                "ownership violation: reserved native runtime ABI symbol '" +
                function.name + "'");
        if (function.name == "main" && function_names.count("main"))
            throw std::runtime_error(
                "ownership violation: native main cannot be overloaded");
        if (function.name == "main" && !function.generic_parameters.empty())
            throw std::runtime_error(
                "ownership violation: native entrypoint main cannot be generic");
        std::unordered_set<std::string> generic_names;
        for (const auto& generic : function.generic_parameters) {
            require_identifier(generic.name, "generic parameter");
            if (generic.name == "_" || generic_names.count(generic.name))
                throw std::runtime_error(
                    "ownership violation: duplicate or reserved generic parameter '" +
                    generic.name + "'");
            // A named default may reference only an already-declared slot,
            // never itself or a forward declaration.
            if (!generic.default_type.empty() &&
                    generic.default_type != "int" &&
                    !generic_names.count(generic.default_type))
                throw std::runtime_error(
                    "ownership violation: forward or unknown generic default '" +
                    generic.default_type + "'");
            generic_names.insert(generic.name);
        }
        for (const auto& parameter : function.parameters) {
            require_identifier(parameter.name, "parameter");
            switch (parameter.mode) {
                case Mode::Value:
                case Mode::Borrow:
                case Mode::BorrowMut:
                    break;
                default:
                    throw std::runtime_error(
                        "ownership violation: unknown parameter ownership mode");
            }
            if (!parameter.class_name.empty() && !parameter.generic_type.empty())
                throw std::runtime_error(
                    "ownership violation: parameter has both class and generic type");
            if (!parameter.class_name.empty())
                require_identifier(parameter.class_name, "parameter class");
            if (!parameter.generic_type.empty()) {
                require_identifier(parameter.generic_type, "parameter generic");
                if (!generic_names.count(parameter.generic_type))
                    throw std::runtime_error(
                        "ownership violation: parameter references unknown generic '" +
                        parameter.generic_type + "'");
            }
        }
        // Overload identity is caller-visible value arity, not parameter
        // types, return types, generic arity, or an internal receiver. Direct
        // Program constructors bypass the parser's declaration-table gate:
        // ignoring emplace(false) would resolve a call against the first
        // duplicate while codegen still emits both conflicting declarations.
        const auto slot = native_function_slot(function.name, function.parameters.size());
        if (!functions.emplace(slot, &function).second)
            throw std::runtime_error(
                "ownership violation: duplicate callable user arity '" + slot + "'");
        function_names.insert(function.name);
    }
    std::unordered_map<std::string, std::unordered_set<std::string>> class_fields;
    std::unordered_map<std::string, std::unordered_set<std::string>> class_arrays;
    std::unordered_map<std::string, std::unordered_set<std::string>> class_finals;
    std::unordered_map<std::string, std::unordered_set<std::string>> class_public;
    std::unordered_set<std::string> class_names;
    for (const auto& cls : program.classes) {
        require_identifier(cls.name, "class");
        for (const auto& parameter : cls.constructor_parameters)
            require_identifier(parameter, "constructor parameter");
        for (const auto& field : cls.fields)
            require_identifier(field.name, "class field");
        if (!class_names.insert(cls.name).second || function_names.count(cls.name))
            throw std::runtime_error(
                "ownership violation: duplicate class/function symbol '" + cls.name + "'");
        std::unordered_set<std::string> fields, arrays, finals, published;
        std::unordered_set<std::string> constructor_parameters;
        for (const auto& parameter : cls.constructor_parameters)
            if (!constructor_parameters.insert(parameter).second)
                throw std::runtime_error(
                    "ownership violation: duplicate class constructor parameter '" +
                    parameter + "'");

        std::function<void(const Expression&)> validate_constructor_expression;
        validate_constructor_expression = [&](const Expression& expr) {
            using K = Expression::Kind;
            switch (expr.kind) {
                case K::Integer:
                    return;
                case K::Variable:
                    if (!constructor_parameters.count(expr.name))
                        reject(expr, "ownership violation: unknown class constructor parameter '" +
                                     expr.name + "'");
                    return;
                case K::Negate:
                    validate_constructor_expression(*expr.left);
                    return;
                case K::Add:
                case K::Subtract:
                case K::Multiply:
                    validate_constructor_expression(*expr.left);
                    validate_constructor_expression(*expr.right);
                    return;
                default:
                    reject(expr,
                        "ownership violation: class field initializer requires pure scalar constructor expression");
            }
        };
        for (const auto& field : cls.fields) {
            if (!fields.insert(field.name).second)
                throw std::runtime_error(
                    "ownership violation: duplicate class field '" + field.name + "'");
            if (field.int_array) arrays.insert(field.name);
            else validate_constructor_expression(*field.initializer_expression);
            if (field.final_field) finals.insert(field.name);
            if (field.public_member) published.insert(field.name);
        }
        class_fields.emplace(cls.name, std::move(fields));
        class_arrays.emplace(cls.name, std::move(arrays));
        class_finals.emplace(cls.name, std::move(finals));
        class_public.emplace(cls.name, std::move(published));
    }

    // Direct Program callers bypass Parser::validate_program(). Reconstruct
    // the actor symbol/capability boundary before LLVM creates generated actor
    // symbols or indexes actor maps. Otherwise an unknown spawn type reaches
    // actorInits.at(), and a failed actor find_if is dereferenced by codegen.
    std::unordered_map<std::string, const ActorDecl*> actor_types;
    for (const auto& actor : program.actors) {
        require_identifier(actor.name, "actor");
        if (!actor.on_message.exported)
            throw std::runtime_error(
                "ownership violation: native actor handler must be exported");
        for (const auto& field : actor.fields)
            require_identifier(field.name, "actor field");
        if (!actor_types.emplace(actor.name, &actor).second ||
                class_names.count(actor.name) || function_names.count(actor.name))
            throw std::runtime_error(
                "ownership violation: duplicate actor/class/function symbol '" +
                actor.name + "'");

        const auto& method = actor.on_message;
        if (method.name != "on_message" || method.parameters.size() != 1 ||
                method.returns_void || !method.result ||
                !method.generic_parameters.empty() || method.scalar_while ||
                !method.locals.empty() || !method.statements.empty() ||
                !method.early_returns.empty() || !method.actor_spawns.empty() ||
                !method.actor_sends.empty() || !method.actor_stops.empty() ||
                !method.actor_actions.empty() || method.caught_throw_value ||
                !method.catch_binding.empty() || !method.caught_throw_defers.empty() ||
                method.has_declared_effects)
            throw std::runtime_error(
                "ownership violation: malformed direct-AST actor handler");

        const auto& parameter = method.parameters.front();
        if (parameter.mode != Mode::Value || !parameter.class_name.empty() ||
                !parameter.generic_type.empty())
            throw std::runtime_error(
                "ownership violation: native actor handler requires one scalar value parameter");

        Locals actor_locals;
        std::unordered_set<std::string> actor_fields;
        const Functions no_functions;
        for (const auto& field : actor.fields) {
            if (!field.initializer || field.heap_owned || field.int_array ||
                    !field.class_name.empty() || field.new_class || field.move_class ||
                    !field.constructor_arguments.empty() || !field.move_source.empty())
                throw std::runtime_error(
                    "ownership violation: native actor state requires scalar initialized fields");
            if (!actor_fields.insert(field.name).second)
                throw std::runtime_error(
                    "ownership violation: duplicate actor field '" + field.name + "'");
            Locals no_locals;
            check_value(*field.initializer, no_locals, no_functions);
            LocalOwnership state;
            state.mutable_owner = field.field_mutable;
            actor_locals.emplace(field.name, std::move(state));
        }
        if (!actor_fields.insert(parameter.name).second)
            throw std::runtime_error(
                "ownership violation: actor parameter shadows field '" +
                parameter.name + "'");
        actor_locals.emplace(parameter.name, LocalOwnership{});
        for (const auto& write : method.actor_field_writes) {
            const auto field = std::find_if(actor.fields.begin(), actor.fields.end(),
                [&](const LocalBinding& item) { return item.name == write.field; });
            if (field == actor.fields.end() || !field->field_mutable)
                throw std::runtime_error(
                    "ownership violation: actor write targets immutable or unknown field '" +
                    write.field + "'");
            check_value(*write.expression, actor_locals, functions);
        }
        check_value(*method.result, actor_locals, functions);

        if (function_names.count("oreslang_actor_" + actor.name + "_init") ||
                function_names.count("oreslang_actor_" + actor.name + "_on_message"))
            throw std::runtime_error(
                "ownership violation: conflicting generated actor symbol");
    }

    const auto contains_callable_invocation = [&](const Expression* root) {
        std::function<bool(const Expression*)> visit;
        visit = [&](const Expression* expr) -> bool {
            if (!expr) return false;
            if (expr->kind == Expression::Kind::Call ||
                    expr->kind == Expression::Kind::ArrayCall)
                return true;
            if (visit(expr->left.get()) || visit(expr->right.get()))
                return true;
            for (const auto& argument : expr->arguments)
                if (visit(argument.get())) return true;
            return false;
        };
        return visit(root);
    };

    for (const auto& function : program.functions) {
        if (function.caught_throw_value)
            require_identifier(function.catch_binding, "catch");

        if (function.has_declared_effects) {
            if (!function.actor_spawns.empty() || !function.actor_sends.empty() ||
                    !function.actor_stops.empty() ||
                    !function.actor_field_writes.empty())
                throw std::runtime_error(
                    "ownership violation: using effects on actor actions requires trusted actor effect summaries");
            if (function.caught_throw_value)
                throw std::runtime_error(
                    "ownership violation: using effects on caught throw requires trusted exceptional summaries");

            for (const auto& binding : function.locals) {
                const bool allocates_class_array =
                    binding.new_class && class_arrays.count(binding.class_name) &&
                    !class_arrays.at(binding.class_name).empty();
                if ((binding.heap_owned || binding.int_array ||
                        allocates_class_array) &&
                        !function.declared_effects.has(FunctionEffect::Alloc))
                    throw std::runtime_error(
                        "ownership violation: owned allocation violates using signature without alloc");
                if (contains_callable_invocation(binding.initializer.get()))
                    throw std::runtime_error(
                        "ownership violation: using effects require transitive call summaries");
            }

            std::function<void(const Statement&)> validate_statement_effects;
            validate_statement_effects = [&](const Statement& stmt) {
                if (stmt.kind == Statement::Kind::ArrayCommitFilled) {
                    if (!function.declared_effects.has(FunctionEffect::Alloc))
                        throw std::runtime_error(
                            "ownership violation: filled array commit violates using signature without alloc");
                    if (contains_callable_invocation(stmt.array_commit_length.get()))
                        throw std::runtime_error(
                            "ownership violation: filled array commit requires pure scalar length");
                    for (const auto& item : stmt.array_commit_values)
                        if (contains_callable_invocation(item.get()))
                            throw std::runtime_error(
                                "ownership violation: filled array commit requires pure scalar fills");
                }
                if (contains_callable_invocation(stmt.expression.get()) ||
                        (stmt.branch_local &&
                         contains_callable_invocation(
                             stmt.branch_local->initializer.get())) ||
                        contains_callable_invocation(stmt.condition_lhs.get()) ||
                        contains_callable_invocation(stmt.condition_rhs.get()))
                    throw std::runtime_error(
                        "ownership violation: using effects require transitive call summaries");
                for (const auto& nested : stmt.then_statements)
                    validate_statement_effects(nested);
                for (const auto& nested : stmt.else_statements)
                    validate_statement_effects(nested);
            };
            for (const auto& stmt : function.statements)
                validate_statement_effects(stmt);

            for (const auto& guard : function.early_returns)
                if (contains_callable_invocation(guard.lhs.get()) ||
                        contains_callable_invocation(guard.rhs.get()) ||
                        contains_callable_invocation(guard.result.get()))
                    throw std::runtime_error(
                        "ownership violation: using effects require transitive call summaries");
            if (contains_callable_invocation(function.result.get()))
                throw std::runtime_error(
                    "ownership violation: using effects require transitive call summaries");
        }

        std::unordered_set<std::string> lexical_names;
        for (const auto& param : function.parameters)
            if (!lexical_names.insert(param.name).second)
                throw std::runtime_error(
                    "ownership violation: duplicate parameter '" + param.name + "'");

        std::unordered_set<std::string> live_actors;
        std::unordered_set<std::string> actor_references;
        const bool has_actor_effects = !function.actor_spawns.empty() ||
            !function.actor_sends.empty() || !function.actor_stops.empty();
        if (has_actor_effects &&
                (function.name != "main" || !function.parameters.empty() ||
                 function.returns_void || !function.early_returns.empty() ||
                 !function.statements.empty()))
            throw std::runtime_error(
                "ownership violation: source actor bootstrap requires zero-argument int main without early returns");

        for (const auto& spawn : function.actor_spawns) {
            require_identifier(spawn.name, "actor reference");
            require_identifier(spawn.actor_type, "actor type");
            if (spawn.declared_type.kind != SourceType::Kind::ActorRef ||
                    spawn.declared_type.argument != spawn.actor_type)
                throw std::runtime_error(
                    "ownership violation: actor spawn declaration/type mismatch");
            if (!actor_types.count(spawn.actor_type))
                throw std::runtime_error(
                    "ownership violation: unknown actor type '" + spawn.actor_type + "'");
            if (!actor_references.insert(spawn.name).second ||
                    lexical_names.count(spawn.name))
                throw std::runtime_error(
                    "ownership violation: duplicate actor reference '" + spawn.name + "'");
            live_actors.insert(spawn.name);
        }
        for (const auto& binding : function.locals) {
            require_identifier(binding.name, "local");
            if ((binding.new_class || binding.move_class) &&
                    !class_names.count(binding.class_name))
                throw std::runtime_error(
                    "ownership violation: unknown class '" + binding.class_name + "'");
            if (binding.move_class && !lexical_names.count(binding.move_source))
                throw std::runtime_error(
                    "ownership violation: unknown class move source '" +
                    binding.move_source + "'");
            if (binding.new_class) {
                const auto cls = std::find_if(program.classes.begin(), program.classes.end(),
                    [&](const ClassDecl& item) { return item.name == binding.class_name; });
                if (cls == program.classes.end() ||
                        binding.constructor_arguments.size() !=
                            cls->constructor_parameters.size())
                    throw std::runtime_error(
                        "ownership violation: native class constructor argument arity mismatch for '" +
                        binding.class_name + "'");
            }
            if (actor_references.count(binding.name))
                throw std::runtime_error(
                    "ownership violation: scalar local shadows actor reference '" +
                    binding.name + "'");
            if (!lexical_names.insert(binding.name).second)
                throw std::runtime_error(
                    "ownership violation: duplicate owned local '" + binding.name + "'");
        }
        for (const auto& action : function.actor_actions) {
            if (action.kind == ActorAction::Kind::Send) {
                const auto& send = function.actor_sends.at(action.index);
                if (!actor_references.count(send.reference))
                    throw std::runtime_error(
                        "ownership violation: unknown actor message receiver '" +
                        send.reference + "'");
                if (!live_actors.count(send.reference))
                    throw std::runtime_error(
                        "ownership violation: actor message sent after stop '" +
                        send.reference + "'");
            } else {
                const auto& stop = function.actor_stops.at(action.index);
                if (!live_actors.erase(stop))
                    throw std::runtime_error(
                        "ownership violation: invalid or repeated actor stop '" +
                        stop + "'");
            }
        }
        if (!live_actors.empty())
            throw std::runtime_error(
                "ownership violation: actor reference must be stopped before return");
    }
    for (const auto& function : program.functions) {
        Locals locals;
        for (const auto& param : function.parameters) {
            if (!param.class_name.empty()) {
                if (!class_fields.count(param.class_name))
                    throw std::runtime_error("unknown parameter class '" + param.class_name + "'");
                if (!class_arrays.at(param.class_name).empty())
                    throw std::runtime_error("ownership violation: Array<int> class cannot be borrowed or passed as a parameter");
                if (param.mode == Mode::Value)
                    throw std::runtime_error("ownership violation: by-value class parameter '" +
                        param.name + "' requires exclusive borrow or proven move/drop lowering");
            }
            LocalOwnership entry;
            entry.mode = param.mode;
            // Native by-value scalar parameters occupy independently owned,
            // mutable stack slots; borrowed parameters retain caller loans.
            // Do not silently confer that mutability on const local bindings.
            entry.mutable_owner = param.mode == Mode::Value;
            entry.class_name = param.class_name;
            if (!param.class_name.empty()) {
                entry.class_fields = class_fields.at(param.class_name);
                entry.class_array_fields = class_arrays.at(param.class_name);
                entry.final_fields = class_finals.at(param.class_name);
                entry.public_fields = class_public.at(param.class_name);
            }
            locals.emplace(param.name, std::move(entry));
        }
        for (const auto& binding : function.locals) {
            LocalOwnership entry;
            entry.heap_owned = binding.heap_owned;
            entry.int_array = binding.int_array;
            entry.mutable_owner = binding.owner_mutable;
            entry.class_name = binding.class_name;
            if (binding.int_array) {
                if (!binding.owner_mutable || binding.heap_owned || binding.initializer)
                    throw std::runtime_error("invalid native Array<int> owner");
            } else if (binding.new_class) {
                // Pure syntax alone is insufficient: experimental owned-int
                // operands carry fallible region reads. Do not let one unwind
                // an incompletely constructed object before drop glue exists.
                std::function<void(const Expression&)> validate_scalar;
                validate_scalar = [&](const Expression& expr) {
                    using K = Expression::Kind;
                    if (expr.kind == K::Integer) return;
                    if (expr.kind == K::Variable) {
                        const auto it = locals.find(expr.name);
                        if (it == locals.end() || it->second.heap_owned ||
                                it->second.int_array || !it->second.class_name.empty())
                            reject(expr, "native scalar constructor arguments require plain stack i64 values");
                        check_value(expr, locals, functions);
                        return;
                    }
                    if (expr.kind == K::Negate && expr.left) {
                        validate_scalar(*expr.left); return;
                    }
                    if ((expr.kind == K::Add || expr.kind == K::Subtract ||
                            expr.kind == K::Multiply) && expr.left && expr.right) {
                        validate_scalar(*expr.left);
                        validate_scalar(*expr.right);
                        return;
                    }
                    reject(expr, "native scalar constructor argument requires pure integer expression");
                };
                for (const auto& arg : binding.constructor_arguments)
                    validate_scalar(*arg);
                entry.class_fields = class_fields.at(binding.class_name);
                entry.class_array_fields = class_arrays.at(binding.class_name);
                entry.final_fields = class_finals.at(binding.class_name);
                entry.public_fields = class_public.at(binding.class_name);
                if (!entry.class_array_fields.empty() && !binding.owner_mutable)
                    throw std::runtime_error("ownership violation: class with Array<int> fields requires let mut owner");
            } else if (binding.move_class) {
                // Transfer the whole class's unique array-owned places, not
                // individual fields. A source with moved fields or live
                // borrows is rejected below before any IR is generated.
                if (!class_arrays.at(binding.class_name).empty() &&
                        !binding.owner_mutable)
                    throw std::runtime_error("ownership violation: moved Array<int> class requires let mut owner");
                const auto found = locals.find(binding.move_source);
                if (found == locals.end() || found->second.class_name != binding.class_name)
                    throw std::runtime_error("ownership violation: class move type mismatch at byte " +
                        std::to_string(binding.offset));
                auto& source = found->second;
                if (source.mode != Mode::Value || source.moved ||
                        source.exclusive_loan || source.shared_loans != 0 ||
                        !source.moved_fields.empty())
                    throw std::runtime_error("ownership violation: class move requires live unborrowed owner at byte " +
                        std::to_string(binding.offset));
                entry.class_fields = source.class_fields;
                entry.class_array_fields = source.class_array_fields;
                entry.final_fields = source.final_fields;
                entry.public_fields = source.public_fields;
                source.moved = true;
            } else {
                check_value(*binding.initializer, locals, functions);
            }
            locals.emplace(binding.name, std::move(entry));
        }
        std::vector<const Statement*> deferred_effects;
        // Provenance for an array field is tracked as a single compile-time
        // shadow handle in this first native slice. Branch-specific moves
        // require phi ownership slots, so reject the entire mixed case.
        const bool conditional_effects = std::any_of(function.statements.begin(),
            function.statements.end(), [](const Statement& stmt) {
                return stmt.kind == Statement::Kind::Conditional;
            });
        auto check_effect = [&](const Statement& stmt, Locals& state) {
            if (stmt.kind == Statement::Kind::ScopedLocal) {
                const LocalBinding& local = *stmt.branch_local;
                if (state.count(local.name))
                    throw std::runtime_error("ownership violation: branch local shadows live owner at byte " +
                        std::to_string(local.offset));
                check_value(*local.initializer, state, functions);
                state.emplace(local.name, LocalOwnership{});
                return;
            }
            if (stmt.kind == Statement::Kind::ArrayCommitFilled) {
                const auto found = state.find(stmt.receiver);
                if (found == state.end() || found->second.class_name.empty())
                    throw std::runtime_error("ownership violation: filled array commit requires named class owner");
                auto& recv = found->second;
                if (recv.class_array_fields.size() != 4)
                    throw std::runtime_error("ownership violation: filled array commit requires exactly four owned Array<int> fields");
                // This source-reachable intrinsic writes all four array slots.
                // Owning the whole class does not grant access to private slots.
                for (const auto& field : recv.class_array_fields)
                    if (!recv.public_fields.count(field))
                        throw std::runtime_error("ownership violation: private class field '" +
                            recv.class_name + "." + field +
                            "' is not accessible to filled array commit");
                if (recv.mode != Mode::Value || !recv.mutable_owner ||
                        recv.moved || !recv.moved_fields.empty() ||
                        recv.exclusive_loan || recv.shared_loans != 0)
                    throw std::runtime_error("ownership violation: filled array commit requires live exclusive mutable whole class");
                if (conditional_effects)
                    throw std::runtime_error("ownership violation: filled array commit requires straight-line ownership CFG");
                // Evaluate every input as a pure scalar before allocation.
                // Calls, borrows and moves cannot be hidden in length/fills.
                check_pure_guard(*stmt.array_commit_length, state, functions);
                if (stmt.array_commit_values.size() != 4)
                    throw std::runtime_error("invalid compiler-owned filled array commit arity");
                for (const auto& v : stmt.array_commit_values)
                    check_pure_guard(*v, state, functions);
                return;
            }
            if (stmt.kind == Statement::Kind::FieldWrite) {
                auto& recv = checked_field(stmt.receiver, stmt.field, stmt.offset, state);
                if (recv.final_fields.count(stmt.field))
                    throw std::runtime_error("ownership violation: cannot reassign final class field");
                if (recv.class_array_fields.count(stmt.field)) {
                    const Expression& move = *stmt.expression;
                    if (move.kind != Expression::Kind::Take || !move.left ||
                            move.left->kind != Expression::Kind::Variable)
                        reject(move, "ownership violation: Array<int> field requires direct rt take of lexical owner");
                    if (conditional_effects)
                        reject(move, "native Array<int> field move requires straight-line ownership CFG");
                    if (recv.mode != Mode::Value || !recv.mutable_owner ||
                            recv.exclusive_loan || recv.shared_loans != 0)
                        reject(move, "ownership violation: Array<int> destination requires exclusive let mut class");
                    LocalOwnership& src = local(*move.left, state);
                    if (!src.int_array || src.mode != Mode::Value ||
                            !src.mutable_owner || src.moved ||
                            src.exclusive_loan || src.shared_loans != 0)
                        reject(move, "ownership violation: Array<int> field transfer requires live unique lexical source");
                    // The old field is released only during the verified
                    // infallible native handle swap; the source becomes dead.
                    src.moved = true;
                    return;
                }
                if (recv.mode == Mode::Borrow)
                    throw std::runtime_error("ownership violation: cannot mutate through shared class borrow");
                if (recv.mode == Mode::Value && !recv.mutable_owner)
                    throw std::runtime_error("ownership violation: class mutation requires let mut owner");
                if (recv.exclusive_loan || recv.shared_loans != 0)
                    throw std::runtime_error("ownership violation: class field write conflicts with active borrow");
                check_value(*stmt.expression, state, functions);
                recv.moved_fields.erase(stmt.field);
            } else if (stmt.kind == Statement::Kind::Call) {
                const Expression& call = *stmt.expression;
                if (call.kind == Expression::Kind::ArrayCall) {
                    if (!call.left)
                        reject(call, "native Array<int> mutation requires a named owner");
                    array_owner(*call.left, state, true);
                    const std::size_t arity = call.name == "add" ? 1 :
                                              call.name == "set" ? 2 : 0;
                    if (!arity || call.arguments.size() != arity)
                        reject(call, "native Array<int> effect supports only add(value) or set(index,value)");
                    for (const auto& arg : call.arguments) check_value(*arg, state, functions);
                } else if (call.kind == Expression::Kind::Call) {
                    check_call(call, state, functions);
                } else {
                    throw std::runtime_error("invalid native call statement");
                }
            } else {
                throw std::runtime_error("ownership violation: unsupported conditional arm effect");
            }
        };
        for (const auto& stmt : function.statements) {
            if (stmt.kind == Statement::Kind::Defer) {
                deferred_effects.push_back(&stmt);
                continue;
            }
            if (stmt.kind == Statement::Kind::Conditional) {
                // A condition cannot change ownership; analyze each arm
                // independently and require compatible resulting states.
                // Until phi ownership/drop flags exist, the checker must
                // reject divergent moves instead of silently picking a path.
                check_pure_guard(*stmt.condition_lhs, locals, functions);
                check_pure_guard(*stmt.condition_rhs, locals, functions);
                Locals then_state = locals;
                Locals else_state = locals;
                for (const auto& arm : stmt.then_statements)
                    check_effect(arm, then_state);
                for (const auto& arm : stmt.else_statements)
                    check_effect(arm, else_state);
                // Scoped i64 locals have complete lexical lifetime in their
                // own arm; they may not influence the predecessor join or
                // be observable on later exit cleanup paths.
                for (const auto& arm : stmt.then_statements)
                    if (arm.kind == Statement::Kind::ScopedLocal)
                        then_state.erase(arm.branch_local->name);
                for (const auto& arm : stmt.else_statements)
                    if (arm.kind == Statement::Kind::ScopedLocal)
                        else_state.erase(arm.branch_local->name);
                for (const auto& item : then_state) {
                    const auto found = else_state.find(item.first);
                    if (found == else_state.end() ||
                            item.second.moved != found->second.moved ||
                            item.second.moved_fields != found->second.moved_fields ||
                            item.second.shared_loans != found->second.shared_loans ||
                            item.second.exclusive_loan != found->second.exclusive_loan)
                        throw std::runtime_error("ownership violation: conditional CFG predecessor ownership mismatch at byte " +
                            std::to_string(stmt.offset));
                }
                locals = std::move(then_state);
                continue;
            }
            check_effect(stmt, locals);
        }
        // Actor sends transfer scalar payloads in lexical send/stop order.
        // Do not let the backend load moved locals or leak raw borrow pointers.
        for (const auto& action : function.actor_actions) {
            if (action.kind == ActorAction::Kind::Send) {
                const auto& send = function.actor_sends.at(action.index);
                check_value(*send.payload, locals, functions);
            }
        }
        auto check_deferred_exit = [&](Locals& path) {
            for (auto it = deferred_effects.rbegin(); it != deferred_effects.rend(); ++it) {
                const Statement& cleanup = **it;
                if (cleanup.receiver.empty()) {
                    check_call(*cleanup.expression, path, functions);
                    continue;
                }
                auto& recv = checked_field(cleanup.receiver, cleanup.field,
                                           cleanup.offset, path);
                if (recv.final_fields.count(cleanup.field))
                    throw std::runtime_error("ownership violation: cannot reassign final class field");
                if (recv.class_array_fields.count(cleanup.field))
                    throw std::runtime_error("ownership violation: cannot reassign owned Array<int> class field");
                if (recv.mode == Mode::Borrow)
                    throw std::runtime_error("ownership violation: cannot mutate through shared class borrow");
                if (recv.mode == Mode::Value && !recv.mutable_owner)
                    throw std::runtime_error("ownership violation: class mutation requires let mut owner");
                if (recv.exclusive_loan || recv.shared_loans != 0)
                    throw std::runtime_error("ownership violation: class field write conflicts with active borrow");
                if (function.caught_throw_value)
                    check_caught_throw_scalar_read(*cleanup.expression, path, functions);
                else
                    check_value(*cleanup.expression, path, functions);
                recv.moved_fields.erase(cleanup.field);
            }
        };
        if (function.caught_throw_value) {
            // Type-check payload before unwind; each scoped field cleanup
            // is verified at its LIFO execution point.
            check_caught_throw_scalar_read(*function.caught_throw_value, locals, functions);
            for (auto it = function.caught_throw_defers.rbegin();
                 it != function.caught_throw_defers.rend(); ++it) {
                const auto& effect = *it;
                auto& recv = checked_field(effect.receiver, effect.field,
                                           effect.offset, locals);
                if (recv.final_fields.count(effect.field))
                    throw std::runtime_error("ownership violation: cannot reassign final class field");
                if (recv.class_array_fields.count(effect.field))
                    throw std::runtime_error("ownership violation: cannot reassign owned Array<int> class field");
                if (recv.mode == Mode::Borrow || (recv.mode == Mode::Value && !recv.mutable_owner))
                    throw std::runtime_error("ownership violation: caught throw defer requires mutable receiver");
                if (recv.exclusive_loan || recv.shared_loans)
                    throw std::runtime_error("ownership violation: caught throw defer conflicts with active borrow");
                check_caught_throw_scalar_read(*effect.expression, locals, functions);
                recv.moved_fields.erase(effect.field);
            }
            if (!locals.emplace(function.catch_binding, LocalOwnership{}).second)
                throw std::runtime_error("native caught throw binding shadows live local");
        }
        const Locals cfg_entry = locals;
        // Analyze each taken return independently. Even an explicit scalar
        // rt take in one return branch does not move that value in the
        // fallthrough path, which has a different live ownership state.
        for (const auto& guard : function.early_returns) {
            check_pure_guard(*guard.lhs, locals, functions);
            check_pure_guard(*guard.rhs, locals, functions);
            Locals taken = locals;
            check_value(*guard.result, taken, functions);
            check_deferred_exit(taken);
        }
        if (function.result) {
            if (function.caught_throw_value)
                check_caught_throw_scalar_read(*function.result, locals, functions);
            else
                check_value(*function.result, locals, functions);
        }
        check_deferred_exit(locals);
        // Build the real guarded-return edge graph and run the reusable
        // field-sensitive fixed-point verifier. This currently models the
        // admitted return-only source CFG. The lattice also handles general
        // diamonds and loops (tested separately), but does not admit new
        // branch-local constructors, exceptional edges or suspension yet.
        ownership::Graph cfg;
        for (const auto& item : cfg_entry) {
            const auto& name = item.first;
            const auto& owner = item.second;
            ownership::PlaceState state;
            state.possible = owner.moved ? ownership::kMoved : ownership::kOwned;
            state.shared_loans = static_cast<unsigned>(owner.shared_loans);
            state.exclusive_loan = owner.exclusive_loan;
            if (owner.class_name.empty()) {
                cfg.entry.emplace(name, state);
            } else {
                for (const auto& field : owner.class_fields) {
                    auto field_state = state;
                    if (owner.moved_fields.count(field))
                        field_state.possible = ownership::kMoved;
                    cfg.entry.emplace(name + "." + field, field_state);
                }
            }
        }
        using OpKind = ownership::Operation::Kind;
        using Ops = std::vector<ownership::Operation>;
        std::function<void(const Expression&, Ops&)> add_effects;
        add_effects = [&](const Expression& expr, Ops& operations) {
            if (expr.kind == Expression::Kind::Variable) {
                const auto found = cfg_entry.find(expr.name);
                const bool object = found != cfg_entry.end() && !found->second.class_name.empty();
                operations.push_back({object ? OpKind::ReadObject : OpKind::Read, expr.name});
                return;
            }
            if (expr.kind == Expression::Kind::ArrayCall) {
                const std::string place = expr.left->kind == Expression::Kind::Field
                    ? expr.left->left->name + "." + expr.left->name : expr.left->name;
                operations.push_back({OpKind::Read, place});
                for (const auto& arg : expr.arguments)
                    add_effects(*arg, operations);
                return;
            }
            if (expr.kind == Expression::Kind::Field) {
                if (expr.left && expr.left->kind == Expression::Kind::Field &&
                        expr.name == "size") {
                    operations.push_back({OpKind::Read,
                        expr.left->left->name + "." + expr.left->name});
                    return;
                }
                if (!expr.left || expr.left->kind != Expression::Kind::Variable)
                    reject(expr, "native CFG requires named field receiver");
                const auto owner = cfg_entry.find(expr.left->name);
                operations.push_back({OpKind::Read,
                    owner != cfg_entry.end() && owner->second.int_array
                        ? expr.left->name : expr.left->name + "." + expr.name});
                return;
            }
            if (expr.kind == Expression::Kind::Take && expr.left) {
                if (expr.left->kind == Expression::Kind::Variable) {
                    operations.push_back({OpKind::Move, expr.left->name});
                    return;
                }
                if (expr.left->kind == Expression::Kind::Field) {
                    if (!expr.left->left ||
                            expr.left->left->kind != Expression::Kind::Variable)
                        reject(expr, "native CFG requires named class field take");
                    operations.push_back({OpKind::Move,
                        expr.left->left->name + "." + expr.left->name});
                    return;
                }
            }
            if (expr.left) add_effects(*expr.left, operations);
            if (expr.right) add_effects(*expr.right, operations);
            for (const auto& argument : expr.arguments)
                add_effects(*argument, operations);
        };
        auto add_cleanup_effects = [&](const Statement& cleanup, Ops& operations) {
            if (!cleanup.receiver.empty())
                operations.push_back({OpKind::Read, cleanup.receiver + "." + cleanup.field});
            add_effects(*cleanup.expression, operations);
        };
        const std::size_t guard_count = function.early_returns.size();
        cfg.blocks.resize(guard_count * 2 + 1);
        for (std::size_t i = 0; i < guard_count; ++i) {
            const auto& guard = function.early_returns[i];
            auto& condition = cfg.blocks[2 * i];
            add_effects(*guard.lhs, condition.operations);
            add_effects(*guard.rhs, condition.operations);
            condition.successors = {2 * i + 1, 2 * i + 2};
            add_effects(*guard.result, cfg.blocks[2 * i + 1].operations);
            // Preserve deferred call inputs after each early-return value.
            for (auto it = deferred_effects.rbegin(); it != deferred_effects.rend(); ++it)
                add_cleanup_effects(**it, cfg.blocks[2 * i + 1].operations);
        }
        if (function.result)
            add_effects(*function.result, cfg.blocks[guard_count * 2].operations);
        for (auto it = deferred_effects.rbegin(); it != deferred_effects.rend(); ++it)
            add_cleanup_effects(**it, cfg.blocks[guard_count * 2].operations);
        // Every reachable terminal CFG path must end with an explicit
        // conditional drop of the experimental heap-owned places. The
        // emitter backs each DropIfOwned with a runtime live flag; scalar
        // stack locals and trivially destructible class fields need none.
        for (auto& block : cfg.blocks)
            if (block.successors.empty())
                for (const auto& entry : cfg_entry)
                    if (entry.second.heap_owned)
                        block.operations.push_back({OpKind::DropIfOwned, entry.first});
        ownership::verify(cfg);

    }
}

} // namespace oreslang
