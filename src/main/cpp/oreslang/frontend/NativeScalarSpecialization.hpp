#pragma once

#include "oreslang/frontend/CallableSelector.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace oreslang {

// Closed, compiler-derived stage-1 native generic ABI. The native scalar type
// checker currently admits only concrete int for generic type arguments;
// it does NOT admit arbitrary traits, nominal generics, or runtime reification.
// The suffix cannot collide with a lexical source identifier ('$' is reserved).
//
// This plan is deliberately rebuilt by the emitter from the post-validation
// Program rather than accepted as source-authored metadata. One declaration
// gets exactly one concrete i64 specialization, reused by all matching calls.
inline std::unordered_map<std::string, std::string>
freeze_native_i64_specializations(const Program& program) {
    constexpr std::size_t kMaxSpecializations = 256;
    constexpr std::size_t kMaxGenericParameters = 32;
    std::unordered_map<std::string, std::string> selected;
    std::unordered_set<std::string> concrete_symbols;
    for (const auto& fn : program.functions) {
        if (fn.generic_parameters.empty()) continue;
        if (selected.size() == kMaxSpecializations)
            throw std::runtime_error("native AOT generic specialization count exceeded");
        if (!fn.method_owner.empty() || fn.generic_parameters.size() > kMaxGenericParameters)
            throw std::runtime_error("native AOT generic method or arity not supported");
        std::unordered_set<std::string> known;
        const auto canonical = [](const std::string& name) {
            if (name.empty() || name.size() > 128) return false;
            const auto first = [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            };
            const auto rest = [&](char c) {
                return first(c) || (c >= '0' && c <= '9');
            };
            if (!first(name.front())) return false;
            for (char c : name) if (!rest(c)) return false;
            return true;
        };
        // Check the declaration shape rather than trusting a previously
        // serialized "specialization approved" boolean.
        for (const auto& param : fn.generic_parameters) {
            if (!canonical(param.name) || known.contains(param.name))
                throw std::runtime_error("invalid native AOT generic parameter identity");
            if (!param.default_type.empty() && param.default_type != "int" &&
                    !known.contains(param.default_type))
                throw std::runtime_error("invalid native AOT generic default dependency");
            known.insert(param.name);
        }
        for (const auto& param : fn.parameters) {
            if (!param.generic_type.empty() &&
                    (!known.contains(param.generic_type) ||
                     !param.class_name.empty() || !param.sum_type.empty()))
                throw std::runtime_error("unresolved native AOT generic parameter ABI");
        }
        // Only int is a concrete generic type in the existing native checker.
        // Encoding the complete ordered tuple makes this a real fixed symbol
        // rather than a runtime generic name/registry lookup.
        std::string suffix = "$atto_g" + std::to_string(fn.generic_parameters.size());
        for (std::size_t i = 0; i < fn.generic_parameters.size(); ++i)
            suffix += "$int";
        const std::string selector = native_function_slot(fn.name, fn.parameters.size());
        if (!selected.emplace(selector, suffix).second)
            throw std::runtime_error("duplicate native AOT generic specialization identity");
        if (!concrete_symbols.insert(selector + suffix).second)
            throw std::runtime_error("duplicate native AOT specialized symbol");
    }
    return selected;
}

} // namespace oreslang
