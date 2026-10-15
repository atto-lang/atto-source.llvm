#include "oreslang/cli/Flags2Env.hpp"

#include <cstdlib>
#include <initializer_list>
#include <string>

// Compile the actual source-owned native CLI helper independently of LLVM.
// It must reject unsupported environment values without changing output.
int main() {
    constexpr auto key = "ORES_TEST_ENV_FLAG";
    if (unsetenv(key) != 0) return 1;
    bool state = true;
    if (!read_env_flag(key, state) || !state) return 2;
    if (setenv(key, "", 1) != 0) return 3;
    state = false;
    if (!read_env_flag(key, state) || state) return 4;
    for (const char* value : {"true", "1", "yes"}) {
        if (setenv(key, value, 1) != 0) return 5;
        state = false;
        if (!read_env_flag(key, state) || !state) return 6;
    }
    for (const char* value : {"false", "0", "no"}) {
        if (setenv(key, value, 1) != 0) return 7;
        state = true;
        if (!read_env_flag(key, state) || state) return 8;
    }
    for (const char* value : {"TRUE", "False", "2", "off", " yes", "yes ", "maybe"}) {
        if (setenv(key, value, 1) != 0) return 9;
        for (bool initial : {false, true}) {
            state = initial;
            if (read_env_flag(key, state) || state != initial) return 10;
        }
    }
    std::string huge(65536, 't');
    if (setenv(key, huge.c_str(), 1) != 0) return 11;
    state = true;
    if (read_env_flag(key, state) || !state) return 12;
    if (unsetenv(key) != 0) return 13;
    return 0;
}
