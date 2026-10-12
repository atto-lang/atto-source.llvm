#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oreslang {
namespace {

enum class Kind { Identifier, Integer, Symbol, End };
struct Token {
    Kind kind;
    std::string_view text;
    std::size_t offset;
};

bool digit(char ch) { return ch >= '0' && ch <= '9'; }
bool identifier_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
bool identifier_part(char ch) { return identifier_start(ch) || digit(ch); }

bool reserved(std::string_view name) {
    // Keep these reserved even while their grammar is unsupported.
    static constexpr std::string_view words[] = {
        "define", "class", "module", "contract", "conforms", "namespace",
        "import", "from", "as", "extends", "implements", "try", "catch",
        "finally", "end", "fi", "if", "do", "else", "then", "new",
        "spawn", "stop", "done", "await", "rt", "async", "generator",
        "nlex", "trap", "actor", "isoactor", "def", "fnc", "routine",
        "for", "of", "loop", "block", "break", "continue", "yield",
        "super", "elseif", "elif", "switch", "match", "matches", "is",
        "default", "eq", "neq", "nb", "select", "readch", "writech",
        "cb", "shared", "untrusted", "type", "types", "typeof",
        "interface", "trait", "struct", "impl", "abstract", "void",
        "static", "pub", "private", "return", "defer", "val", "const",
        "let", "mut", "self", "true", "false", "null", "obj", "arr",
        "int", "owned", "declare", "receive", "Message", "bool", "string",
        "Array", "List"
    };
    return std::find(std::begin(words), std::end(words), name) != std::end(words);
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        for (;;) {
            if (at_ == source_.size()) return {Kind::End, {}, at_};
            const unsigned char ch = static_cast<unsigned char>(source_[at_]);
            if (std::isspace(ch)) { ++at_; continue; }
            if (source_[at_] == '#') {
                const auto end = source_.find('\n', at_ + 1);
                at_ = end == std::string_view::npos ? source_.size() : end;
                continue;
            }
            if (source_.substr(at_, 2) == "//") {
                const auto end = source_.find('\n', at_ + 2);
                at_ = end == std::string_view::npos ? source_.size() : end;
                continue;
            }
            if (source_.substr(at_, 2) == "/*") {
                const auto start = at_;
                at_ += 2;
                std::size_t depth = 1;
                while (depth != 0) {
                    if (at_ == source_.size()) {
                        throw std::runtime_error("unterminated block comment at byte "
                                                 + std::to_string(start));
                    }
                    if (source_.substr(at_, 2) == "/*") { at_ += 2; ++depth; }
                    else if (source_.substr(at_, 2) == "*/") { at_ += 2; --depth; }
                    else ++at_;
                }
                continue;
            }
            break;
        }
        const std::size_t start = at_;
        const char ch = source_[at_];
        if (identifier_start(ch)) {
            ++at_;
            while (at_ < source_.size() && identifier_part(source_[at_])) ++at_;
            return {Kind::Identifier, source_.substr(start, at_ - start), start};
        }
        if (digit(ch)) {
            ++at_;
            while (at_ < source_.size() && (digit(source_[at_]) || source_[at_] == '_')) {
                ++at_;
            }
            return {Kind::Integer, source_.substr(start, at_ - start), start};
        }
        if (std::string_view("()[]{},:;+-*=.<>").find(ch) != std::string_view::npos) {
            ++at_;
            return {Kind::Symbol, source_.substr(start, 1), start};
        }
        throw std::runtime_error("unsupported Oreslang token at byte "
                                 + std::to_string(start));
    }

private:
    std::string_view source_;
    std::size_t at_ = 0;
};

std::unique_ptr<Expression> make_expression(Expression::Kind kind,
                                             std::size_t offset) {
    auto result = std::make_unique<Expression>();
    result->kind = kind;
    result->offset = offset;
    return result;
}

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), token_(lexer_.next()) {}

    Program parse() {
        Program program;
        bool wrapped_module = false;
        if (accept("define")) {
            if (accept("module")) {
                program.module = identifier();
                accept("as");
                wrapped_module = true;
            } else if (accept("actor")) {
                program.actors.push_back(actor());
            } else {
                fail("expected 'module' or 'actor'");
            }
        }
        while (token_.kind != Kind::End
                && !(wrapped_module && token_.text == "end")) {
            if (accept("declare")) {
                program.message_patterns.push_back(message_pattern());
            } else if (accept("define")) {
                require("actor");
                program.actors.push_back(actor());
            } else {
                program.functions.push_back(function());
            }
        }
        if (wrapped_module) require("end");
        if (token_.kind != Kind::End) fail("unexpected trailing source");
        if (program.functions.empty() && program.actors.empty()
            && program.message_patterns.empty())
            fail("expected a function or actor");
        validate(program);
        return program;
    }

private:
    MessageShape message_shape() {
        if (accept("bool")) return {MessageShape::Kind::Boolean, {}};
        if (accept("int")) return {MessageShape::Kind::Integer, {}};
        if (accept("string")) return {MessageShape::Kind::String, {}};
        MessageShape shape;
        std::string_view close;
        if (accept("(")) {
            shape.kind = MessageShape::Kind::Tuple;
            close = ")";
        } else if (accept("Array")) {
            shape.kind = MessageShape::Kind::Array;
            require("[");
            close = "]";
        } else if (accept("List")) {
            shape.kind = MessageShape::Kind::List;
            require("[");
            close = "]";
        } else {
            fail("expected message shape (bool, int, string, tuple, Array or List)");
        }
        if (accept(close))
            fail("empty structural message pattern is not supported");
        do {
            shape.elements.push_back(message_shape());
        } while (accept(","));
        require(close);
        return shape;
    }

    MessagePattern message_pattern() {
        MessagePattern pattern;
        pattern.offset = token_.offset;
        require("type");
        require("match");
        pattern.name = identifier();
        require("as");
        while (token_.kind != Kind::End && token_.text != "end") {
            pattern.alternatives.push_back(message_shape());
            accept(";");
        }
        if (pattern.alternatives.empty())
            fail("message match contract must declare at least one alternative");
        require("end");
        return pattern;
    }

    Actor actor() {
        Actor value;
        value.offset = token_.offset;
        value.name = identifier();
        if (accept("<")) {
            require("messages");
            require("=");
            value.messages_type = identifier();
            require(">");
        }
        require("as");
        const bool braced = accept("{");
        // The first native actor subset deliberately admits only immutable
        // scalar fields and read-only methods; no async or user destructors.
        while (token_.text == "const" || token_.text == "val" || token_.text == "let") {
            LocalBinding field;
            field.offset = token_.offset;
            field.field_mutable = token_.text == "let";
            advance();
            field.name = identifier();
            require(":");
            require("int");
            require("=");
            field.initializer = expression();
            require(";");
            value.fields.push_back(std::move(field));
        }
        if (value.fields.empty() && value.messages_type.empty())
            fail("actor requires at least one scalar field");
        while (token_.text == "fnc" || token_.text == "routine"
               || token_.text == "pub" || token_.text == "receive") {
            value.methods.push_back(function(true));
        }
        if (value.methods.empty()) fail("actor requires at least one read-only method");
        if (braced) require("}");
        else require("end");
        return value;
    }

    Function function(bool actor_method = false) {
        Function value;
        value.offset = token_.offset;
        value.exported = accept("pub");
        value.is_receiver = actor_method && accept("receive");
        if (value.is_receiver) {
            value.name = "receive";
            value.exported = true;
        } else {
            if (!accept("fnc") && !accept("routine")) fail("expected fnc or routine");
            value.name = identifier();
        }
        require("(");
        if (value.is_receiver) {
            require("Message");
            value.parameters.push_back(identifier());
            require(")");
        } else {
            if (!accept(")")) {
                do {
                    require("int");
                    value.parameters.push_back(identifier());
                } while (accept(","));
                require(")");
            }
            require(":");
            require("int");
        }
        require("{");
        bool scalar_local_seen = false;
        while (token_.text == "const" || token_.text == "val") {
            LocalBinding binding;
            binding.offset = token_.offset;
            advance();
            binding.name = identifier();
            const bool has_explicit_type = accept(":");
            SourceType declared_type;
            bool actor_ref_type = false;
            if (has_explicit_type) {
                if (accept("ActorRef")) {
                    require("<");
                    declared_type.kind = SourceType::Kind::ActorRef;
                    declared_type.argument = identifier();
                    require(">");
                    actor_ref_type = true;
                } else if (accept("Future")) {
                    // Reserve a genuine typed Future category without
                    // creating a fake synchronous future value or await.
                    require("<");
                    declared_type.kind = SourceType::Kind::Future;
                    if (accept("int")) declared_type.argument = "int";
                    else declared_type.argument = identifier();
                    require(">");
                    fail("Future<T> requires native continuation lowering");
                } else {
                    binding.heap_owned = accept("owned");
                    require("int");
                }
            }
            require("=");
            if (token_.text == "await")
                fail("await requires native compiler continuation lowering");
            if (accept("spawn")) {
                if (actor_method || scalar_local_seen
                    || (has_explicit_type && !actor_ref_type))
                    fail("actor spawn requires ActorRef<T> or inferred reference in main");
                ActorSpawnBinding actor_ref;
                actor_ref.offset = binding.offset;
                actor_ref.name = std::move(binding.name);
                actor_ref.actor_type = identifier();
                actor_ref.declared_type.kind = SourceType::Kind::ActorRef;
                actor_ref.declared_type.argument = actor_ref_type
                    ? declared_type.argument : actor_ref.actor_type;
                if (actor_ref.declared_type.argument != actor_ref.actor_type)
                    fail("ActorRef<T> type mismatch with spawned actor");
                require("(");
                require(")");
                require(";");
                value.actor_spawns.push_back(std::move(actor_ref));
            } else {
                if (actor_ref_type)
                    fail("ActorRef<T> requires spawn initializer");
                if (!has_explicit_type) fail("scalar local requires ': int'");
                scalar_local_seen = true;
                binding.initializer = expression();
                require(";");
                value.locals.push_back(std::move(binding));
            }
        }
        if (!actor_method) {
            if (accept("await")) {
                value.ready_await_actor = identifier();
                require(".");
                require("get_ready");
                require("(");
                require(")");
                require(";");
            }
            while (token_.kind == Kind::Identifier && !reserved(token_.text)) {
                ActorSend send;
                send.offset = token_.offset;
                send.reference = identifier();
                require(".");
                require("send");
                require("(");
                send.payload = expression();
                require(")");
                require(";");
                value.actor_sends.push_back(std::move(send));
            }
            while (accept("stop")) {
                value.actor_stops.push_back(identifier());
                require(";");
            }
        }
        if (token_.text == "await")
            fail("await requires native compiler continuation lowering");
        // Restricted synchronous actor methods can update declared `let` fields.
        while (actor_method && token_.kind == Kind::Identifier
               && !reserved(token_.text)) {
            FieldAssignment assignment;
            assignment.offset = token_.offset;
            assignment.field = identifier();
            require("=");
            assignment.value = expression();
            require(";");
            value.assignments.push_back(std::move(assignment));
        }
        if (value.is_receiver && token_.text == "}") {
            value.result = make_expression(Expression::Kind::Integer, token_.offset);
            value.result->integer = 0;
        } else {
            require("return");
            value.result = expression();
            require(";");
        }
        require("}");
        return value;
    }

    std::unique_ptr<Expression> expression() { return additive(); }

    std::unique_ptr<Expression> additive() {
        auto left = product();
        while (token_.text == "+" || token_.text == "-") {
            const auto offset = token_.offset;
            const auto op = token_.text;
            advance();
            auto expr = make_expression(op == "+" ? Expression::Kind::Add
                                                 : Expression::Kind::Subtract, offset);
            expr->left = std::move(left);
            expr->right = product();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> product() {
        auto left = unary();
        while (token_.text == "*") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::Multiply, offset);
            expr->left = std::move(left);
            expr->right = unary();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> unary() {
        if (token_.text == "-") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::Negate, offset);
            expr->left = unary();
            return expr;
        }
        return primary();
    }

    std::unique_ptr<Expression> primary() {
        const auto offset = token_.offset;
        if (accept("(")) {
            auto inner = expression();
            require(")");
            return inner;
        }
        if (token_.text == "true" || token_.text == "false") {
            auto expr = make_expression(Expression::Kind::Boolean, offset);
            expr->integer = token_.text == "true" ? 1 : 0;
            advance();
            return expr;
        }
        if (token_.kind == Kind::Integer) {
            auto expr = make_expression(Expression::Kind::Integer, offset);
            std::string digits(token_.text);
            if (digits.front() == '_' || digits.back() == '_'
                    || digits.find("__") != std::string::npos) {
                fail("malformed integer separator");
            }
            digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
            const auto parsed = std::from_chars(digits.data(),
                                                 digits.data() + digits.size(),
                                                 expr->integer);
            if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
                fail("integer literal overflows signed 64-bit result");
            }
            advance();
            return expr;
        }
        if (token_.kind != Kind::Identifier || reserved(token_.text)) {
            fail("expected int expression");
        }
        auto expr = make_expression(Expression::Kind::Variable, offset);
        expr->name = identifier();
        if (accept("(")) {
            expr->kind = Expression::Kind::Call;
            if (!accept(")")) {
                do {
                    expr->arguments.push_back(expression());
                } while (accept(","));
                require(")");
            }
        }
        return expr;
    }

    bool accept(std::string_view text) {
        if (token_.text != text) return false;
        advance();
        return true;
    }

    void require(std::string_view text) {
        if (!accept(text)) fail("expected '" + std::string(text) + "'");
    }

    std::string identifier() {
        if (token_.kind != Kind::Identifier || reserved(token_.text)) {
            fail("expected non-keyword identifier");
        }
        std::string result(token_.text);
        advance();
        return result;
    }

    static void validate_expression(const Expression& expr,
                                    const std::unordered_set<std::string>& parameters,
                                    const std::unordered_map<std::string, std::size_t>& functions) {
        switch (expr.kind) {
            case Expression::Kind::Integer:
            case Expression::Kind::Boolean: return;
            case Expression::Kind::Variable:
                if (parameters.count(expr.name) == 0) {
                    throw std::runtime_error("unknown parameter '" + expr.name
                                             + "' at byte " + std::to_string(expr.offset));
                }
                return;
            case Expression::Kind::Call: {
                const auto found = functions.find(expr.name);
                if (found == functions.end() || found->second != expr.arguments.size()) {
                    throw std::runtime_error("unknown function or wrong call arity '"
                                             + expr.name + "' at byte "
                                             + std::to_string(expr.offset));
                }
                for (const auto& arg : expr.arguments) {
                    validate_expression(*arg, parameters, functions);
                }
                return;
            }
            case Expression::Kind::Negate:
                validate_expression(*expr.left, parameters, functions);
                return;
            case Expression::Kind::Add:
            case Expression::Kind::Subtract:
            case Expression::Kind::Multiply:
                validate_expression(*expr.left, parameters, functions);
                validate_expression(*expr.right, parameters, functions);
                return;
        }
        throw std::runtime_error("internal invalid expression kind");
    }

    static std::string shape_key(const MessageShape& shape) {
        switch (shape.kind) {
            case MessageShape::Kind::Boolean: return "bool";
            case MessageShape::Kind::Integer: return "int";
            case MessageShape::Kind::String: return "string";
            default: break;
        }
        const char* label = shape.kind == MessageShape::Kind::Tuple ? "Tuple"
            : shape.kind == MessageShape::Kind::Array ? "Array" : "List";
        std::string text(label);
        text += "[";
        for (const auto& element : shape.elements) {
            if (text.back() != '[') text += ",";
            text += shape_key(element);
        }
        return text + "]";
    }

    static bool contains_boolean(const Expression& e) {
        if (e.kind == Expression::Kind::Boolean) return true;
        if (e.left && contains_boolean(*e.left)) return true;
        if (e.right && contains_boolean(*e.right)) return true;
        for (const auto& arg : e.arguments)
            if (contains_boolean(*arg)) return true;
        return false;
    }

    static bool reads_name(const Expression& e, const std::string& name) {
        if (e.kind == Expression::Kind::Variable && e.name == name) return true;
        if (e.left && reads_name(*e.left, name)) return true;
        if (e.right && reads_name(*e.right, name)) return true;
        for (const auto& arg : e.arguments)
            if (reads_name(*arg, name)) return true;
        return false;
    }

    static void validate(const Program& program) {
        std::unordered_map<std::string, const MessagePattern*> messagePatterns;
        for (const auto& pattern : program.message_patterns) {
            if (!messagePatterns.emplace(pattern.name, &pattern).second)
                throw std::runtime_error("duplicate message match contract '" + pattern.name + "'");
            std::unordered_set<std::string> variants;
            for (const auto& alternative : pattern.alternatives) {
                if (!variants.insert(shape_key(alternative)).second)
                    throw std::runtime_error("duplicate message shape in match '" + pattern.name + "'");
            }
        }
        std::unordered_set<std::string> declaredActors;
        for (const auto& actor : program.actors) declaredActors.insert(actor.name);
        std::unordered_map<std::string, std::size_t> functions;
        for (const auto& fn : program.functions) {
            if (!functions.emplace(fn.name, fn.parameters.size()).second) {
                throw std::runtime_error("duplicate function '" + fn.name + "' at byte "
                                         + std::to_string(fn.offset));
            }
            std::unordered_set<std::string> params;
            for (const auto& param : fn.parameters) {
                if (!params.insert(param).second) {
                    throw std::runtime_error("duplicate parameter '" + param
                                             + "' at byte " + std::to_string(fn.offset));
                }
            }
        }
        for (const auto& fn : program.functions) {
            std::unordered_set<std::string> available(fn.parameters.begin(),
                                                      fn.parameters.end());
            if (!fn.actor_spawns.empty() || !fn.actor_stops.empty()
                || !fn.ready_await_actor.empty()) {
                if (fn.name != "main" || !fn.parameters.empty())
                    throw std::runtime_error("source actor spawn requires zero-argument main");
            }
            std::unordered_set<std::string> liveActors;
            std::unordered_map<std::string, std::string> actorTypes;
            for (const auto& spawn : fn.actor_spawns) {
                if (!declaredActors.count(spawn.actor_type))
                    throw std::runtime_error("unknown actor type '" + spawn.actor_type + "'");
                if (!liveActors.insert(spawn.name).second ||
                    !available.insert(spawn.name).second)
                    throw std::runtime_error("duplicate actor reference '" + spawn.name + "'");
                actorTypes.emplace(spawn.name, spawn.actor_type);
            }
            if (!fn.ready_await_actor.empty()) {
                // First source continuation prototype intentionally supports
                // a single linear ActorRef, no scalar locals across yield,
                // and deterministic trusted-main teardown after resumption.
                if (fn.actor_spawns.size() != 1 || !fn.locals.empty() ||
                    !liveActors.count(fn.ready_await_actor))
                    throw std::runtime_error(
                        "await get_ready requires one source actor without suspended locals");
            }
            // Actor references cannot be cast to i64 or used as scalar values.
            for (const auto& actor : liveActors) available.erase(actor);
            for (const auto& local : fn.locals) {
                if (liveActors.count(local.name))
                    throw std::runtime_error("scalar local shadows actor reference '" + local.name + "'");
                // The initializer is checked BEFORE publication: x=x+1 or
                // forward references cannot read uninitialized stack memory.
                validate_expression(*local.initializer, available, functions);
                if (!available.insert(local.name).second) {
                    throw std::runtime_error("duplicate owned local '" + local.name
                                             + "' at byte " + std::to_string(local.offset));
                }
            }
            for (const auto& send : fn.actor_sends) {
                auto ref = actorTypes.find(send.reference);
                if (ref == actorTypes.end())
                    throw std::runtime_error("unknown actor message receiver '" + send.reference + "'");
                const auto actor = std::find_if(program.actors.begin(), program.actors.end(),
                    [&](const Actor& a) { return a.name == ref->second; });
                if (actor == program.actors.end()) throw std::runtime_error("unknown actor type");
                if (!actor->messages_type.empty()) {
                    auto found = messagePatterns.find(actor->messages_type);
                    if (found == messagePatterns.end())
                        throw std::runtime_error("unknown actor message match contract");
                    const MessagePattern& contract = *found->second;
                    if (contract.alternatives.size() != 1 ||
                        (contract.alternatives.front().kind != MessageShape::Kind::Boolean
                         && contract.alternatives.front().kind != MessageShape::Kind::Integer))
                        throw std::runtime_error(
                            "typed structural/union message transport requires tagged envelope lowering");
                    const bool expectedBool =
                        contract.alternatives.front().kind == MessageShape::Kind::Boolean;
                    if (expectedBool && send.payload->kind != Expression::Kind::Boolean)
                        throw std::runtime_error("actor send expects bool message");
                    if (!expectedBool && contains_boolean(*send.payload))
                        throw std::runtime_error("actor send expects int message");
                } else {
                    const auto method = std::find_if(actor->methods.begin(), actor->methods.end(),
                        [](const Function& fn) { return fn.name == "on_message"; });
                    if (method == actor->methods.end() || !method->exported ||
                        method->parameters.size() != 1)
                        throw std::runtime_error("actor send requires pub on_message(int)");
                }
                validate_expression(*send.payload, available, functions);
            }
            validate_expression(*fn.result, available, functions);
            for (const auto& name : fn.actor_stops) {
                if (!liveActors.erase(name))
                    throw std::runtime_error("invalid or repeated actor stop '" + name + "'");
            }
            if (!liveActors.empty())
                throw std::runtime_error("actor reference must be stopped before return");
        }

        std::unordered_set<std::string> actorNames;
        std::unordered_set<std::string> generatedSymbols;
        auto reserveGenerated = [&](const std::string& name) {
            if (functions.count(name) || !generatedSymbols.insert(name).second)
                throw std::runtime_error("conflicting generated actor symbol '" + name + "'");
        };
        for (const auto& actor : program.actors) {
            if (!actor.messages_type.empty()) {
                if (!messagePatterns.count(actor.messages_type))
                    throw std::runtime_error(
                        "unknown actor message match contract '" + actor.messages_type + "'");
                const auto count = std::count_if(actor.methods.begin(), actor.methods.end(),
                    [](const Function& f) { return f.is_receiver; });
                if (count != 1)
                    throw std::runtime_error("typed actor requires exactly one receive(Message m)");
                if (std::any_of(actor.methods.begin(), actor.methods.end(),
                    [](const Function& f) { return f.name == "on_message"; }))
                    throw std::runtime_error(
                        "typed actor must use receive, not on_message");
            } else if (std::any_of(actor.methods.begin(), actor.methods.end(),
                    [](const Function& f) { return f.is_receiver; })) {
                throw std::runtime_error("receive requires actor messages=Contract");
            }
            if (!actorNames.insert(actor.name).second
                || functions.count(actor.name) != 0) {
                throw std::runtime_error("duplicate actor symbol '" + actor.name + "'");
            }
            const std::string prefix = "oreslang_actor_"
                + (program.module.empty() ? "" : program.module + "_")
                + actor.name + "_";
            reserveGenerated(prefix + "init");
            std::unordered_set<std::string> fields;
            std::unordered_set<std::string> mutableFields;
            // Deliberately restrict initializers to closed arithmetic:
            // initialization must not escape the actor turn or call FFI.
            const std::unordered_set<std::string> none;
            const std::unordered_map<std::string, std::size_t> noCalls;
            for (const auto& field : actor.fields) {
                validate_expression(*field.initializer, none, noCalls);
                if (!fields.insert(field.name).second)
                    throw std::runtime_error("duplicate actor field '" + field.name + "'");
                if (field.field_mutable) mutableFields.insert(field.name);
            }
            std::unordered_set<std::string> methods;
            for (const auto& method : actor.methods) {
                if (!methods.insert(method.name).second)
                    throw std::runtime_error("duplicate actor method '" + method.name + "'");
                reserveGenerated(prefix + method.name);
                std::unordered_set<std::string> available = fields;
                for (const auto& param : method.parameters) {
                    if (!available.insert(param).second)
                        throw std::runtime_error("actor parameter shadows field '" + param + "'");
                }
                for (const auto& local : method.locals) {
                    validate_expression(*local.initializer, available, functions);
                    if (!available.insert(local.name).second)
                        throw std::runtime_error("actor local shadows field/parameter '" + local.name + "'");
                }
                if (method.is_receiver) {
                    const std::string& parameter = method.parameters.at(0);
                    if (reads_name(*method.result, parameter) ||
                        std::any_of(method.assignments.begin(), method.assignments.end(),
                            [&](const FieldAssignment& a) {
                                return reads_name(*a.value, parameter);
                            }))
                        throw std::runtime_error(
                            "receive Message payload inspection requires match lowering");
                }
                for (const auto& assignment : method.assignments) {
                    if (!mutableFields.count(assignment.field)) {
                        throw std::runtime_error("cannot assign immutable or unknown actor field '" + assignment.field + "'");
                    }
                    validate_expression(*assignment.value, available, functions);
                }
                validate_expression(*method.result, available, functions);
            }
        }
    }

    void advance() { token_ = lexer_.next(); }
    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(reason + " at byte " + std::to_string(token_.offset));
    }

    Lexer lexer_;
    Token token_;
};

} // namespace

Program parse_program(std::string_view source) {
    return Parser(source).parse();
}

} // namespace oreslang
