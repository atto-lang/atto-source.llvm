#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

// flags-2-env exports these variables using .cli-flags.toml. Read them without
// depending on an ambient TOML parser in the compiler (the upstream wrapper
// owns TOML/argv validation). Invalid inherited values always fail closed.
static bool read_env_flag(const char* key, bool& output) {
    const char* raw = std::getenv(key);
    if (raw == nullptr || *raw == '\0') return true;
    const std::string_view value(raw);
    if (value == "true" || value == "1" || value == "yes") { output = true; return true; }
    if (value == "false" || value == "0" || value == "no") { output = false; return true; }
    std::cerr << "invalid boolean environment value for " << key << '\n';
    return false;
}
