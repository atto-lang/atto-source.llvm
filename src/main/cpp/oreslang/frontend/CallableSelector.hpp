#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

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
        return std::tie(lhs.name, lhs.arity) <
               std::tie(rhs.name, rhs.arity);
    }
};

inline CallableId callable_id(std::string name, std::size_t arity) {
    return CallableId{std::move(name), arity};
}

inline std::string analysis_callable_key(const CallableId& id) {
    return id.name + "/" + std::to_string(id.arity);
}

inline CallableId parse_analysis_callable_key(const std::string& key) {
    const auto slash = key.rfind('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= key.size())
        throw std::invalid_argument(
            "invalid analysis callable key: " + key);

    std::size_t arity = 0;
    for (std::size_t i = slash + 1; i < key.size(); ++i) {
        const char ch = key[i];
        if (ch < '0' || ch > '9')
            throw std::invalid_argument(
                "invalid analysis callable key: " + key);
        const std::size_t digit =
            static_cast<std::size_t>(ch - '0');
        if (arity > (static_cast<std::size_t>(-1) - digit) / 10)
            throw std::invalid_argument(
                "analysis callable arity overflow");
        arity = arity * 10 + digit;
    }
    return CallableId{key.substr(0, slash), arity};
}

// ABI/report boundary only. Native source identifiers cannot contain '$'.
inline std::string native_function_slot(
        const std::string& name,
        std::size_t value_arity) {
    return name + "$arity" + std::to_string(value_arity);
}

inline std::string native_function_slot(const CallableId& id) {
    return native_function_slot(id.name, id.arity);
}

} // namespace oreslang
