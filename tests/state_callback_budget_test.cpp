#include "oreslang/frontend/Parser.hpp"
#include <stdexcept>
#include <string>

static std::string callback(const std::string& expression) {
    return "define actor Counter as state { x: int = 0; } "
           "read(): int with state.read { return using state.read(|s| -> "
           + expression + "); } end";
}

static void rejects(const std::string& expression, const std::string& diagnostic) {
    bool rejected = false;
    try {
        (void)oreslang::parse_program(callback(expression));
    } catch (const std::runtime_error& failure) {
        if (std::string(failure.what()).find(diagnostic) == std::string::npos)
            throw;
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("unbounded state callback was admitted");
}

static std::string sum(unsigned count) {
    std::string result = "s.x";
    while (--count) result += "+s.x";
    return result;
}

int main() {
    rejects(std::string(100000, '(') + "s.x" + std::string(100000, ')'),
            "state callback parser nesting limit");
    rejects(std::string(100000, '-') + "s.x", "state callback parser nesting limit");
    rejects(sum(100000), "state callback token budget");
    rejects(sum(66), "state callback expression depth limit");
    rejects(std::string(64, '(') + "s.x" + std::string(64, ')'),
            "state callback parser nesting limit");
    (void)oreslang::parse_program(callback(sum(65)));
    (void)oreslang::parse_program(callback(std::string(63, '(') + "s.x" + std::string(63, ')')));
    (void)oreslang::parse_program(callback("s.x"));
}
