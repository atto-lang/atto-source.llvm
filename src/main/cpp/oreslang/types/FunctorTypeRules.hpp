#pragma once

// Compiler-side *type algebra*, not a native guest functor implementation.
// Native sum lowering currently handles scalar payloads only. Never promote
// host-only tests of these rules to LLVM machine-code/ownership conformance.
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang::types::functor_v1 {

struct TypeError : std::runtime_error {
    std::string category;
    TypeError(std::string code, std::string reason)
        : std::runtime_error(std::move(reason)), category(std::move(code)) {}
};

struct Type {
    std::string name;
    std::vector<Type> arguments;

    bool operator==(const Type& other) const {
        return name == other.name && arguments == other.arguments;
    }
    bool operator!=(const Type& other) const { return !(*this == other); }
    std::string str() const {
        std::string result = name;
        if (!arguments.empty()) {
            result += "<";
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                if (i) result += ",";
                result += arguments[i].str();
            }
            result += ">";
        }
        return result;
    }
};

inline void require_arity(const Type& type, std::size_t count) {
    if (type.arguments.size() != count)
        throw TypeError("invalid-type-arity",
                        type.name + " requires " + std::to_string(count) + " type argument(s)");
}

class Parser final {
    std::string_view input_;
    std::size_t pos_ = 0;

    void spaces() {
        while (pos_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }
    bool accept(char ch) {
        spaces();
        if (pos_ < input_.size() && input_[pos_] == ch) { ++pos_; return true; }
        return false;
    }
    Type node(unsigned depth) {
        if (depth > 32) throw TypeError("invalid-nested-type", "functor nesting exceeds 32 layers");
        spaces();
        const auto first = pos_;
        if (pos_ == input_.size() ||
                !(std::isalpha(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_'))
            throw TypeError("invalid-nested-type", "expected a named type");
        while (pos_ < input_.size()) {
            const auto ch = static_cast<unsigned char>(input_[pos_]);
            if (!(std::isalnum(ch) || ch == '_' || ch == '.')) break;
            ++pos_;
        }
        Type type{std::string(input_.substr(first, pos_ - first)), {}};
        if (type.name == "_")
            throw TypeError("unresolved-nested-generic", "type inference hole is not a resolved return type");
        if (type.name == "string") type.name = "String";
        if (accept('<')) {
            if (accept('>'))
                throw TypeError("invalid-type-arity", "empty generic argument list");
            do {
                type.arguments.push_back(node(depth + 1));
            } while (accept(','));
            if (!accept('>'))
                throw TypeError("invalid-nested-type", "missing closing '>'");
        }
        if (type.name == "Option") require_arity(type, 1);
        if (type.name == "Result" || type.name == "Either") require_arity(type, 2);
        if (type.name == "Tree") require_arity(type, 1);
        if ((type.name == "int" || type.name == "bool" || type.name == "String" ||
                type.name == "void") && !type.arguments.empty())
            throw TypeError("invalid-type-arity", "scalar type cannot take generic arguments");
        return type;
    }
public:
    explicit Parser(std::string_view source) : input_(source) {}
    Type parse() {
        if (input_.size() > 4096)
            throw TypeError("invalid-nested-type", "type signature exceeds 4096 bytes");
        auto value = node(0);
        spaces();
        if (pos_ != input_.size())
            throw TypeError("invalid-nested-type", "unexpected trailing type tokens");
        return value;
    }
};

inline Type parse_type(std::string_view source) { return Parser(source).parse(); }

inline void require_functor(const Type& source) {
    if (source.name == "Option") require_arity(source, 1);
    else if (source.name == "Result") require_arity(source, 2);
    else throw TypeError("invalid-functor-receiver", "expected Option<T> or Result<T,E>");
}

inline void require_input(const Type& source, const Type& callback_input) {
    if (source.arguments[0] != callback_input)
        throw TypeError("wrong-callback-input-type", "callback input does not match functor payload");
}

// map never collapses layers. The callback result may itself be an ADT.
inline Type map(const Type& source, const Type& callback_input, const Type& callback_result) {
    require_functor(source);
    require_input(source, callback_input);
    Type output = source;
    output.arguments[0] = callback_result;
    return output;
}

inline Type map_err(const Type& source, const Type& callback_input, const Type& callback_result) {
    if (source.name != "Result")
        throw TypeError("invalid-functor-receiver", "map_err requires Result<T,E>");
    require_arity(source, 2);
    if (source.arguments[1] != callback_input)
        throw TypeError("wrong-callback-input-type", "map_err callback must receive the nominal error type");
    return Type{"Result", {source.arguments[0], callback_result}};
}

inline Type and_then(const Type& source, const Type& callback_input, const Type& callback_result) {
    require_functor(source);
    require_input(source, callback_input);
    if (callback_result.name != source.name)
        throw TypeError("wrong-return-type", "and_then callback must return the same outer functor");
    require_functor(callback_result);
    if (source.name == "Result" && source.arguments[1] != callback_result.arguments[1])
        throw TypeError("incompatible-nominal-error-domain",
                        "Result.and_then cannot silently change nominal error domains");
    return callback_result;
}

// flatten eliminates precisely one layer and cannot erase/change error types.
inline Type flatten(const Type& source) {
    require_functor(source);
    const Type& inner = source.arguments[0];
    if (inner.name != source.name)
        throw TypeError("invalid-flatten-constructor", "flatten requires a matching inner functor");
    require_functor(inner);
    if (source.name == "Result" && source.arguments[1] != inner.arguments[1])
        throw TypeError("incompatible-nominal-error-domain",
                        "Result.flatten requires identical nominal error types");
    return inner;
}

// Both directions preserve every deeper constructor without implicit recursion.
inline Type transpose(const Type& source) {
    require_functor(source);
    const auto& inner = source.arguments[0];
    if (source.name == "Option" && inner.name == "Result") {
        require_arity(inner, 2);
        return Type{"Result", {Type{"Option", {inner.arguments[0]}}, inner.arguments[1]}};
    }
    if (source.name == "Result" && inner.name == "Option") {
        require_arity(inner, 1);
        return Type{"Option", {Type{"Result", {inner.arguments[0], source.arguments[1]}}}};
    }
    throw TypeError("invalid-transpose-constructor",
                    "transpose requires Option<Result<T,E>> or Result<Option<T>,E>");
}

enum class OwnerState { Owned, SharedBorrow, MutableBorrow, Moved };

// Pre-lowering admission policy only. This cannot replace LLVM loan tracking,
// tagged-value destruction, allocator counters, or sanitizers on guest binaries.
inline void check_affine_rebuild(OwnerState state, bool affine_payload) {
    if (state == OwnerState::Moved)
        throw TypeError("use-after-move", "cannot transform a consumed functor owner");
    if (affine_payload && state != OwnerState::Owned)
        throw TypeError("affine-borrow-escape", "cannot reconstruct an affine payload from a loan");
}

} // namespace oreslang::types::functor_v1
