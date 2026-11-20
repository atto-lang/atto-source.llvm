#pragma once

#include "oreslang/frontend/GpuLaunchSourceMir.hpp"
#include "oreslang/runtime/GpuTypedLaunchIr.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::gpu::source_link {

// Strictly source-declared types. This bridge does NOT type-check host
// arguments, validate kernel bodies, grant device access or emit device code.
struct Parameter {
    std::string name;
    typed_ir::Parameter type;
};
struct Signature {
    std::string symbol;
    std::vector<Parameter> parameters;
    bool returns_void = true;
    bool source_body_scanned_only = true;
};
// No source-only value may have the layout/type of typed_ir::Kernel:
// typed_ir::Kernel.effects are accepted by the host preflight, while this
// declaration has NOT had its body or transitive effects type-checked.
// AccessHint is merely syntactic access implied by declared parameter views.
enum class AccessHint { DeviceRead, DeviceWrite };
struct SourceShape {
    std::string symbol;
    std::vector<typed_ir::Parameter> parameter_shapes;
    std::vector<AccessHint> access_hints;
    bool returns_void = true;
    [[nodiscard]] static constexpr bool effects_verified() noexcept { return false; }
    [[nodiscard]] static constexpr bool device_code_available() noexcept { return false; }
};
struct Linked {
    SourceShape source_shape;
    source_mir::Candidate launch_shape;
    // These are literal source spellings; their referenced values are NOT
    // resolved to the host type checker or an authenticated provider snapshot.
    std::vector<std::string> unresolved_host_arguments;
    [[nodiscard]] static constexpr bool executable() noexcept { return false; }
    [[nodiscard]] static constexpr bool host_arguments_typed() noexcept { return false; }
    [[nodiscard]] static constexpr bool kernel_body_verified() noexcept { return false; }
};

[[noreturn]] inline void reject(const char* code, const char* message) {
    throw std::invalid_argument(std::string(code) + ": " + message);
}
inline bool supported(typed_ir::Parameter value) {
    switch (value) {
        case typed_ir::Parameter::F32:
        case typed_ir::Parameter::I64:
        case typed_ir::Parameter::U64:
        case typed_ir::Parameter::ReadF32:
        case typed_ir::Parameter::WriteF32: return true;
        default: return false;
    }
}
inline Signature checked_signature(Signature s) {
    if (!source_mir::source_symbol(s.symbol))
        reject("GPU-LAUNCH-001", "invalid source kernel name");
    if (s.parameters.size() > 64 || !s.returns_void ||
            !s.source_body_scanned_only)
        reject("GPU-LAUNCH-001", "unsupported kernel arity, return or body authority");
    std::unordered_set<std::string> names;
    for (const auto& parameter : s.parameters) {
        if (!source_mir::source_symbol(parameter.name) ||
                parameter.name.find('.') != std::string::npos ||
                !names.insert(parameter.name).second)
            reject("GPU-LAUNCH-001", "duplicate or invalid kernel parameter");
        if (!supported(parameter.type))
            reject("GPU-LAUNCH-002", "unsupported kernel parameter type");
    }
    return s;
}
inline std::vector<Signature> checked_catalog(std::vector<Signature> signatures) {
    if (signatures.size() > 512)
        reject("GPU-LAUNCH-001", "too many source kernels");
    std::unordered_set<std::string> names;
    for (auto& s : signatures) {
        s = checked_signature(std::move(s));
        if (!names.insert(s.symbol).second)
            reject("GPU-LAUNCH-001", "duplicate source GPU kernel");
    }
    return signatures;
}
inline SourceShape signature_to_source_shape(const Signature& input) {
    const auto s = checked_signature(input);
    SourceShape result;
    result.symbol = s.symbol;
    result.returns_void = s.returns_void;
    bool reads = false, writes = false;
    for (const auto& p : s.parameters) {
        result.parameter_shapes.push_back(p.type);
        reads |= p.type == typed_ir::Parameter::ReadF32;
        writes |= p.type == typed_ir::Parameter::WriteF32;
    }
    if (reads) result.access_hints.push_back(AccessHint::DeviceRead);
    if (writes) result.access_hints.push_back(AccessHint::DeviceWrite);
    return result;
}
inline Linked link(const std::vector<Signature>& declarations,
                   source_mir::Candidate candidate) {
    // Validate the untrusted source IR *before* any name or arity lookup.
    auto launch = source_mir::source_candidate(std::move(candidate));
    const auto catalog = checked_catalog(declarations);
    const auto found = std::find_if(catalog.begin(), catalog.end(),
        [&](const Signature& s) { return s.symbol == launch.kernel; });
    if (found == catalog.end())
        reject("GPU-LAUNCH-001", "source GPU kernel not declared in this unit");
    if (launch.ordered_arguments.size() != found->parameters.size())
        reject("GPU-LAUNCH-002", "GPU source launch arity differs from declaration");
    auto resolved = signature_to_source_shape(*found);
    auto arguments = launch.ordered_arguments;
    return {std::move(resolved), std::move(launch), std::move(arguments)};
}
} // namespace oreslang::gpu::source_link
