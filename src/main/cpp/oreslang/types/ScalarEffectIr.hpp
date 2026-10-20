#pragma once
// Narrow, fail-closed native scalar source-to-effect IR v1 exporter.
// This is *model-only* arithmetic work, NOT authenticated source provenance,
// memory-safety certification or a hardware WCET/stack guarantee.
#include "oreslang/frontend/Parser.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang {
inline std::string emit_native_scalar_effect_ir_v1(const Program& program) {
    if (!program.module.empty() || !program.classes.empty() ||
            !program.actors.empty() || !program.cooperative_loops.empty() ||
            program.functions.empty() || program.functions.size() > 1024)
        throw std::runtime_error("effect IR v1 requires bounded scalar-only module");
    constexpr std::size_t kMaxOperations = 100000;
    constexpr std::size_t kMaxDepth = 64;
    std::size_t total = 0;
    std::unordered_set<std::string> symbols;
    std::vector<std::pair<std::string, std::size_t>> validated;
    for (const auto& function : program.functions) {
        const auto& symbol = function.name;
        if (symbol.empty() || symbol.size() > 256 || symbol == "main" ||
                !(std::isalpha(static_cast<unsigned char>(symbol[0])) || symbol[0] == '_'))
            throw std::runtime_error("effect IR v1 invalid function identifier");
        for (const auto letter : symbol)
            if (!(letter >= 'a' && letter <= 'z') &&
                    !(letter >= 'A' && letter <= 'Z') &&
                    !(letter >= '0' && letter <= '9') && letter != '_')
                throw std::runtime_error("effect IR v1 non-ASCII identifier");
        if (!symbols.insert(symbol).second || function.returns_void ||
                !function.generic_parameters.empty() || !function.parameters.empty() ||
                !function.locals.empty() || !function.statements.empty() ||
                !function.early_returns.empty() || !function.actor_spawns.empty() ||
                !function.actor_sends.empty() || !function.actor_stops.empty() ||
                !function.actor_actions.empty() || !function.actor_field_writes.empty() ||
                function.scalar_while || function.caught_throw_value ||
                !function.caught_throw_defers.empty() || function.has_declared_effects ||
                !function.result)
            throw std::runtime_error("effect IR v1 rejects non-scalar function");
        std::size_t nodes = 0;
        std::vector<std::pair<const Expression*, std::size_t>> work{
                {function.result.get(), 1}};
        while (!work.empty()) {
            const auto item = work.back();
            work.pop_back();
            const auto* expr = item.first;
            if (!expr || item.second > kMaxDepth ||
                    total >= kMaxOperations)
                throw std::runtime_error("effect IR v1 expression budget exceeded");
            ++nodes;
            ++total;
            switch (expr->kind) {
                case Expression::Kind::Integer:
                    if (expr->left || expr->right || !expr->arguments.empty())
                        throw std::runtime_error("effect IR v1 malformed literal");
                    break;
                case Expression::Kind::Add:
                case Expression::Kind::Subtract:
                case Expression::Kind::Multiply:
                    if (!expr->left || !expr->right || !expr->arguments.empty())
                        throw std::runtime_error("effect IR v1 malformed operator");
                    work.push_back({expr->right.get(), item.second + 1});
                    work.push_back({expr->left.get(), item.second + 1});
                    break;
                default:
                    // Includes Negate, Variable, Call, FFI, ownership, arrays.
                    throw std::runtime_error("effect IR v1 unsupported expression");
            }
        }
        validated.emplace_back(symbol, nodes);
    }
    std::sort(validated.begin(), validated.end());
    std::string out = "{\"schema_version\":1,\"functions\":[";
    for (std::size_t i = 0; i < validated.size(); ++i) {
        if (i) out += ',';
        out += "{\"name\":\"";
        out += validated[i].first;
        out += "\",\"operations\":[";
        for (std::size_t j = 0; j < validated[i].second; ++j) {
            if (j) out += ',';
            out += "{\"kind\":\"compute\",\"units\":1}";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}
} // namespace oreslang
