#pragma once

#include <cstddef>
#include <string>
#include <tuple>

namespace oreslang {

struct CallableId {
    std::string name;
    std::size_t arity = 0;

    friend bool operator==(const CallableId& lhs, const CallableId& rhs) {
        return lhs.name == rhs.name && lhs.arity == rhs.arity;
    }
    friend bool operator!=(const CallableId& lhs, const CallableId& rhs) {
        return !(lhs == rhs);
    }
    friend bool operator<(const CallableId& lhs, const CallableId& rhs) {
        return std::tie(lhs.name, lhs.arity) < std::tie(rhs.name, rhs.arity);
    }
};

inline CallableId callable_id(std::string name, std::size_t arity) {
    return CallableId{std::move(name), arity};
}

inline std::string analysis_callable_key(const CallableId& id) {
    return id.name + "/" + std::to_string(id.arity);
}

// This is a compile-time selector/ABI key, never a runtime structural
// overload resolution step. Native source identifiers cannot contain 'inline std::string native_function_slot(const std::string& name, std::size_t value_arity) {
    return name + "$arity" + std::to_string(value_arity);
}

inline std::string native_function_slot(const CallableId& id) {
    return native_function_slot(id.name, id.arity);
}

} // namespace oreslang
.
// A single encoder must be used by the ownership verifier, code generator,
// and exported overloaded symbols to prevent signature/ABI drift.
inline std::string native_function_slot(const std::string& name, std::size_t value_arity) {
    return name + "$arity" + std::to_string(value_arity);
}

} // namespace oreslang
