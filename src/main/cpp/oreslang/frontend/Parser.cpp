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
        "int", "owned"
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
        if (std::string_view("(){}<>|,:;+-*=.").find(ch) != std::string_view::npos) {
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
            if (accept("actor")) {
                program.actor = std::make_unique<ActorDecl>(actor_declaration());
                if (token_.text == "fnc") {
                    program.script = std::make_unique<ActorScript>(
                        actor_script(program.actor->name));
                }
                if (token_.kind != Kind::End) fail("unexpected trailing actor source");
                validate(program);
                return program;
            }
            require("module");
            program.module = identifier();
            accept("as"); // Both 'define module M as' and 'define module M' occur in Java.
            wrapped_module = true;
        }
        while (token_.kind != Kind::End
                && !(wrapped_module && token_.text == "end")) {
            program.functions.push_back(function());
        }
        if (wrapped_module) require("end");
        if (token_.kind != Kind::End) fail("unexpected trailing source");
        if (program.functions.empty()) fail("expected at least one function");
        validate(program);
        return program;
    }

private:
    ActorDecl actor_declaration() {
        ActorDecl actor;
        actor.offset = token_.offset;
        actor.name = identifier();
        require("as");
        while (token_.text == "let" || token_.text == "const" || token_.text == "val") {
            ActorField field;
            field.offset = token_.offset;
            field.mutable_field = token_.text == "let";
            advance();
            field.name = identifier();
            require(":");
            require("int");
            require("=");
            field.initializer = expression();
            require(";");
            actor.fields.push_back(std::move(field));
            if (actor.fields.size() > 4096) fail("actor field capacity exceeds 4096");
        }
        if (actor.fields.empty()) fail("actor requires at least one int field");
        while (token_.text != "end" && token_.kind != Kind::End) {
            actor.methods.push_back(actor_method());
        }
        require("end");
        if (actor.methods.empty()) fail("actor requires at least one method");
        return actor;
    }

    ActorScript actor_script(const std::string& actor_name) {
        ActorScript script;
        script.offset = token_.offset;
        script.actor_name = actor_name;
        require("fnc");
        require("main");
        require("(");
        require(")");
        require(":");
        require("int");
        require("{");
        if (!accept("const") && !accept("val"))
            fail("expected const actor worker binding");
        script.worker_name = identifier();
        require("=");
        script.await_spawn = accept("await");
        require("spawn");
        const std::string selected = identifier();
        if (selected != actor_name)
            fail("source spawn actor must match enclosing actor declaration");
        require("(");
        require(")");
        require(";");
        if (!script.await_spawn) {
            require("await");
            require(script.worker_name);
            require(".");
            require("get_ready");
            require("(");
            require(")");
            require(";");
        }
        // Actor references expose only transport and lifecycle operations,
        // never arbitrary actor class methods.
        require(script.worker_name);
        require(".");
        require("send");
        require("(");
        require("infer");
        require("struct");
        require("{");
        script.message_field = identifier();
        require(":");
        script.argument = expression();
        require("}");
        require(")");
        require(";");
        require("await");
        require(script.worker_name);
        require(".");
        require("get_done");
        require("(");
        require(")");
        require(";");
        require("return");
        require("0");
        require(";");
        require("}");
        script.method = "receive";
        return script;
    }

    ActorMethod actor_method() {
        ActorMethod value;
        value.offset = token_.offset;
        const bool public_method = accept("pub");
        if (token_.text == "receive") {
            if (public_method) fail("receive is actor-local and must not be public");
            advance();
            value.name = "receive";
            require("(");
            require("ActorMail");
            require("<");
            do {
                require("{");
                const std::string field = identifier();
                require(":");
                require("int");
                require("}");
                if (std::find(value.accepted_message_variants.begin(),
                              value.accepted_message_variants.end(), field)
                        != value.accepted_message_variants.end())
                    fail("ambiguous duplicate actor message variant");
                value.accepted_message_variants.push_back(field);
                if (value.accepted_message_variants.size() > 16)
                    fail("actor message union exceeds 16 variants");
            } while (accept("|"));
            value.accepted_message_field = value.accepted_message_variants.front();
            require(">");
            const std::string envelope = identifier();
            require(")");
            require(":");
            require("void");
            require("{");
            current_mail_envelope_ = envelope;
            for (const auto& field : value.accepted_message_variants)
                value.parameters.push_back(field);
            if (value.accepted_message_variants.size() > 1) {
                require("match");
                require(envelope);
                require(".");
                require("value");
                while (accept("is")) {
                    ActorVariantArm arm;
                    arm.offset = token_.offset;
                    require("{");
                    arm.field = identifier();
                    require(":");
                    require("int");
                    require("}");
                    require("-");
                    require(">");
                    require("{");
                    current_mail_field_ = arm.field;
                    while (token_.text == "self") {
                        ActorAssignment assignment;
                        assignment.offset = token_.offset;
                        advance();
                        require(".");
                        assignment.field = identifier();
                        require("=");
                        assignment.value = expression();
                        require(";");
                        arm.assignments.push_back(std::move(assignment));
                    }
                    require("}");
                    value.variant_arms.push_back(std::move(arm));
                }
                require("end");
                // Multi-message actors remain alive across receives. They
                // must be stopped by supervisor/control rather than a match.
                require("return");
                require(";");
                require("}");
            } else {
                // Preserve the original one-shot typed message pilot.
                current_mail_field_ = value.accepted_message_field;
                while (token_.text == "self") {
                    const auto offset = token_.offset;
                    advance();
                    require(".");
                    const bool actor_end = accept("end");
                    const std::string target = actor_end ? "end" : identifier();
                    if (actor_end) {
                        require("(");
                        require(")");
                        require(";");
                        value.ends_after_receive = true;
                        break;
                    }
                    ActorAssignment assignment;
                    assignment.offset = offset;
                    assignment.field = target;
                    require("=");
                    assignment.value = expression();
                    require(";");
                    value.assignments.push_back(std::move(assignment));
                }
                if (!value.ends_after_receive)
                    fail("typed single-message pilot requires self.end() after state handling");
                require("return");
                require(";");
                require("}");
            }
            current_mail_envelope_.clear();
            current_mail_field_.clear();
            value.result = make_expression(Expression::Kind::Integer, value.offset);
            value.result->integer = 0;
            return value;
        }
        if (!accept("fnc") && !accept("routine")) fail("expected actor method");
        value.name = identifier();
        require("(");
        if (!accept(")")) {
            do {
                require("int");
                value.parameters.push_back(identifier());
            } while (accept(","));
            require(")");
        }
        require(":");
        require("int");
        require("{");
        while (token_.text == "self") {
            ActorAssignment assignment;
            assignment.offset = token_.offset;
            advance();
            require(".");
            assignment.field = identifier();
            require("=");
            assignment.value = expression();
            require(";");
            value.assignments.push_back(std::move(assignment));
        }
        require("return");
        value.result = expression();
        require(";");
        require("}");
        return value;
    }

    std::int64_t parse_dispatch_case() {
        const bool negative = accept("-");
        if (token_.kind != Kind::Integer)
            fail("native match/switch supports integer literal cases only");
        std::string digits(token_.text);
        // Literal patterns use the same numeric separator grammar as
        // ordinary int expressions: malformed spellings must not silently
        // become a different, apparently exhaustive match case.
        if (digits.empty() || digits.front() == '_' || digits.back() == '_'
                || digits.find("__") != std::string::npos)
            fail("malformed dispatch integer separator");
        digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
        std::uint64_t raw = 0;
        auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), raw);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()
                || raw > (negative ? UINT64_C(9223372036854775808)
                                   : UINT64_C(9223372036854775807)))
            fail("dispatch integer case out of signed 64-bit range");
        advance();
        if (negative && raw == UINT64_C(9223372036854775808)) return INT64_MIN;
        return negative ? -static_cast<std::int64_t>(raw)
                        : static_cast<std::int64_t>(raw);
    }

    std::unique_ptr<Expression> dispatch(bool procedural) {
        const auto offset = token_.offset;
        const bool is_match = accept("match");
        if (!is_match) require("switch");
        auto expr = make_expression(
                is_match ? Expression::Kind::Match : Expression::Kind::Switch, offset);
        expr->left = expression();
        if (is_match) require("over");
        std::unordered_set<std::int64_t> distinct;
        bool fallback = false;
        while (token_.text != "end" && token_.kind != Kind::End) {
            Expression::Arm arm;
            if (is_match) {
                require("on");
                (void)accept("is");
                if (accept("_")) arm.fallback = true;
                else arm.cases.push_back(parse_dispatch_case());
            } else {
                if (accept("default")) arm.fallback = true;
                else {
                    require("case");
                    do { arm.cases.push_back(parse_dispatch_case()); }
                    while (accept(","));
                }
            }
            // Match wildcard remains source-ordered and makes later arms unreachable.
            // Switch default is a fallback regardless of its source position;
            // explicit cases may follow it, but a second default is invalid.
            if (fallback && (is_match || arm.fallback))
                fail(is_match ? "dispatch arm after catch-all is unreachable"
                              : "duplicate switch default");
            if (arm.fallback) fallback = true;
            for (const auto number : arm.cases)
                if (!distinct.insert(number).second)
                    fail("duplicate dispatch integer case");
            (void)accept("->");
            require("{");
            if (accept("return")) {
                arm.value = expression();
                require(";");
            } else if (!procedural) {
                fail("value match/switch arm must return a value");
            }
            require("}");
            // Braced arms may be separated by commas/semicolons (or whitespace).
            // These are delimiters, never additional empty cases or fallthrough.
            // The formatter canonicalizes them away; both backends must admit them.
            while (accept(";") || accept(",")) {}
            expr->arms.push_back(std::move(arm));
            if (expr->arms.size() > 256) fail("dispatch arm count exceeds 256");
        }
        require("end");
        if (!is_match && fallback) {
            // Preserve the relative order of concrete cases while keeping the
            // only default last for both SSA result and procedural lowering.
            auto default_arm = std::find_if(expr->arms.begin(), expr->arms.end(),
                [](const Expression::Arm& arm) { return arm.fallback; });
            if (default_arm + 1 != expr->arms.end())
                std::rotate(default_arm, default_arm + 1, expr->arms.end());
        }
        if (expr->arms.empty()) fail("match/switch needs at least one arm");
        if (!procedural && !fallback)
            fail("non-exhaustive value match/switch: add catch-all");
        return expr;
    }

    Function function() {
        Function value;
        value.offset = token_.offset;
        value.exported = accept("pub");
        if (!accept("fnc") && !accept("routine")) fail("expected fnc or routine");
        value.name = identifier();
        require("(");
        if (!accept(")")) {
            do {
                require("int");
                value.parameters.push_back(identifier());
            } while (accept(","));
            require(")");
        }
        require(":");
        require("int");
        require("{");
        while (token_.text == "const" || token_.text == "val") {
            LocalBinding binding;
            binding.offset = token_.offset;
            advance();
            binding.name = identifier();
            require(":");
            // 'owned int' is a heap-owned scalar, intentionally explicit.
            // It never decays into the ordinary alloca-backed int type.
            binding.heap_owned = accept("owned");
            require("int");
            require("=");
            binding.initializer = expression();
            require(";");
            value.locals.push_back(std::move(binding));
        }
        bool returned = false;
        while (token_.kind != Kind::End && token_.text != "}") {
            if (accept("do")) {
                Function::DoDispatch item;
                item.dispatch = dispatch(true);
                value.procedural.push_back(std::move(item));
                (void)accept(";");
                continue;
            }
            if (accept("return")) {
                if (accept("do")) {
                    Function::DoDispatch item;
                    item.dispatch = dispatch(true);
                    item.terminal = true;
                    value.procedural.push_back(std::move(item));
                    (void)accept(";");
                } else {
                    value.result = expression();
                    if (!accept(";")
                            && value.result->kind != Expression::Kind::Match
                            && value.result->kind != Expression::Kind::Switch)
                        require(";");
                }
                returned = true;
                break;
            }
            fail("expected 'return' or a supported 'do match/switch' statement");
        }
        if (!returned) fail("native function requires a final return or return do");
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
        if (token_.text == "match" || token_.text == "switch")
            return dispatch(false);
        if (accept("(")) {
            auto inner = expression();
            require(")");
            return inner;
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
        if (accept("self")) {
            require(".");
            auto expr = make_expression(Expression::Kind::ActorField, offset);
            expr->name = identifier();
            return expr;
        }
        if (token_.kind != Kind::Identifier || reserved(token_.text)) {
            fail("expected int expression");
        }
        auto expr = make_expression(Expression::Kind::Variable, offset);
        expr->name = identifier();
        if (!current_mail_envelope_.empty() && expr->name == current_mail_envelope_
            && accept(".")) {
            require("value");
            require(".");
            require(current_mail_field_);
            expr->name = current_mail_field_;
            return expr;
        }
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
                                    const std::unordered_map<std::string, std::size_t>& functions,
                                    const std::unordered_set<std::string>* fields = nullptr) {
        switch (expr.kind) {
            case Expression::Kind::Integer: return;
            case Expression::Kind::Match:
            case Expression::Kind::Switch: {
                validate_expression(*expr.left, parameters, functions, fields);
                for (const auto& arm : expr.arms)
                    if (arm.value)
                        validate_expression(*arm.value, parameters, functions, fields);
                return;
            }
            case Expression::Kind::ActorField:
                if (fields == nullptr || !fields->count(expr.name)) {
                    throw std::runtime_error("unknown or forbidden actor field '" + expr.name
                                             + "' at byte " + std::to_string(expr.offset));
                }
                return;
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
                    validate_expression(*arg, parameters, functions, fields);
                }
                return;
            }
            case Expression::Kind::Negate:
                validate_expression(*expr.left, parameters, functions, fields);
                return;
            case Expression::Kind::Add:
            case Expression::Kind::Subtract:
            case Expression::Kind::Multiply:
                validate_expression(*expr.left, parameters, functions, fields);
                validate_expression(*expr.right, parameters, functions, fields);
                return;
        }
        throw std::runtime_error("internal invalid expression kind");
    }

    static void validate(const Program& program) {
        if (program.actor) {
            const auto& actor = *program.actor;
            std::unordered_set<std::string> fields;
            std::unordered_map<std::string, bool> mutable_fields;
            const std::unordered_set<std::string> empty;
            const std::unordered_map<std::string, std::size_t> no_calls;
            for (const auto& field : actor.fields) {
                // Initializers are pure integer expressions. Actor state
                // cannot escape or be read before the entire record exists.
                validate_expression(*field.initializer, empty, no_calls);
                if (!fields.insert(field.name).second) {
                    throw std::runtime_error("duplicate actor field '" + field.name
                                             + "' at byte " + std::to_string(field.offset));
                }
                mutable_fields.emplace(field.name, field.mutable_field);
            }
            std::unordered_set<std::string> names;
            for (const auto& method : actor.methods) {
                if (method.name == "init" || method.name == "drop"
                        || method.name == "layout_hash" || method.name == "layout_version"
                        || method.name == "program_step"
                        || method.name == "message_schema_hash") {
                    throw std::runtime_error("reserved actor lifecycle method '" + method.name + "'");
                }
                if (!names.insert(method.name).second) {
                    throw std::runtime_error("duplicate actor method '" + method.name + "'");
                }
                std::unordered_set<std::string> params;
                for (const auto& parameter : method.parameters) {
                    if (!params.insert(parameter).second) {
                        throw std::runtime_error("duplicate actor parameter '" + parameter + "'");
                    }
                }
                for (const auto& assignment : method.assignments) {
                    if (!fields.count(assignment.field)) {
                        throw std::runtime_error("unknown actor assignment field '" + assignment.field + "'");
                    }
                    if (!mutable_fields.at(assignment.field)) {
                        throw std::runtime_error("cannot assign immutable actor field '" + assignment.field + "'");
                    }
                    validate_expression(*assignment.value, params, no_calls, &fields);
                }
                validate_expression(*method.result, params, no_calls, &fields);
                if (method.name == "receive" && method.accepted_message_variants.size() > 1) {
                    std::unordered_set<std::string> matched;
                    for (const auto& arm : method.variant_arms) {
                        if (std::find(method.accepted_message_variants.begin(),
                                      method.accepted_message_variants.end(), arm.field)
                                == method.accepted_message_variants.end())
                            throw std::runtime_error("match arm selects undeclared actor message variant '" + arm.field + "'");
                        if (!matched.insert(arm.field).second)
                            throw std::runtime_error("duplicate actor message match arm '" + arm.field + "'");
                        std::unordered_set<std::string> arm_bindings{arm.field};
                        for (const auto& assignment : arm.assignments) {
                            if (!fields.count(assignment.field))
                                throw std::runtime_error("unknown actor assignment field '" + assignment.field + "'");
                            if (!mutable_fields.at(assignment.field))
                                throw std::runtime_error("cannot assign immutable actor field '" + assignment.field + "'");
                            validate_expression(*assignment.value, arm_bindings, no_calls, &fields);
                        }
                    }
                    if (matched.size() != method.accepted_message_variants.size())
                        throw std::runtime_error("non-exhaustive actor message match");
                }
            }
            if (program.script) {
                const auto& script = *program.script;
                const auto method = std::find_if(actor.methods.begin(), actor.methods.end(),
                    [&](const ActorMethod& entry) { return entry.name == "receive"; });
                if (method == actor.methods.end() || method->parameters.size() != 1
                        || !method->ends_after_receive)
                    throw std::runtime_error("actor must declare receive(ActorMail<{field: int}> mail) that terminates after delivery");
                if (method->accepted_message_field != script.message_field)
                    throw std::runtime_error("ActorRef.send mailbox value does not match declared receive payload field");
                validate_expression(*script.argument, empty, no_calls);
            }
            return;
        }
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
            for (const auto& local : fn.locals) {
                // The initializer is checked BEFORE publication: x=x+1 or
                // forward references cannot read uninitialized stack memory.
                validate_expression(*local.initializer, available, functions);
                if (!available.insert(local.name).second) {
                    throw std::runtime_error("duplicate owned local '" + local.name
                                             + "' at byte " + std::to_string(local.offset));
                }
            }
            for (const auto& action : fn.procedural) {
                validate_expression(*action.dispatch, available, functions);
                if (action.terminal) {
                    bool has_fallback = false;
                    for (const auto& arm : action.dispatch->arms) {
                        has_fallback |= arm.fallback;
                        if (!arm.value)
                            throw std::runtime_error(
                                    "return do match/switch requires every selected arm to return");
                    }
                    if (!has_fallback)
                        throw std::runtime_error("non-exhaustive return do match/switch");
                }
            }
            if (fn.result)
                validate_expression(*fn.result, available, functions);
            else if (fn.procedural.empty() || !fn.procedural.back().terminal)
                throw std::runtime_error("native function missing result or terminal do");
        }
    }

    void advance() { token_ = lexer_.next(); }
    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(reason + " at byte " + std::to_string(token_.offset));
    }

    Lexer lexer_;
    Token token_;
    std::string current_mail_envelope_;
    std::string current_mail_field_;
};

} // namespace

Program parse_program(std::string_view source) {
    return Parser(source).parse();
}

} // namespace oreslang
