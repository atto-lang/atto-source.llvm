#include "oreslang/types/NativeReachability.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }
static bool contains(const oreslang::Program& p, const std::string& name, std::size_t arity) {
    for (const auto& f : p.functions) if (f.name == name && f.parameters.size() == arity) return true;
    return false;
}
static const char* helpers =
    "fnc leaf(int x): int { return x + 1; }\n"
    "fnc helper(int x): int { return leaf(x); }\n"
    "fnc helper(): int { return 99; }\n"
    "fnc dead(): int { return 7; }\n";
static const char* actor =
    "define actor Worker as { let total: int = 0; pub fnc on_message(int n): int { "
    "total = helper(n); return total; } }\n";
int main() {
    try {
        unsigned checks = 0;
        for (const bool explicit_root : {false, true}) {
            auto p = oreslang::parse_program(std::string(helpers) + actor + "pub fnc main(): int { return 43; }");
            oreslang::tree_shake_functions(p, explicit_root ? std::vector<std::string>{"main"} : std::vector<std::string>{});
            require(contains(p, "helper", 1) && contains(p, "leaf", 1), "actor callback transitive dependency pruned");
            require(contains(p, "main", 0), "entrypoint pruned");
            require(!contains(p, "helper", 0) && !contains(p, "dead", 0), "unreachable ordinary functions retained");
            checks += 3;
        }
        {
            auto p = oreslang::parse_program(std::string(helpers) + actor);
            oreslang::tree_shake_functions(p, {});
            require(p.functions.size() == 2, "actor-only compilation lost callback roots"); ++checks;
        }
        {
            auto p = oreslang::parse_program("define actor Empty as { const value: int = 1; pub fnc on_message(int n): int { return value + n; } }");
            oreslang::tree_shake_functions(p, {});
            require(p.actors.size() == 1 && p.functions.empty(), "actor-only root needs artificial main"); ++checks;
        }
        {
            auto p = oreslang::parse_program(std::string(helpers) + actor);
            // A malformed AST must fail before publication, rather than
            // silently retaining no code for a scheduler-invoked target.
            p.functions.erase(p.functions.begin() + 1);
            bool denied = false;
            try { oreslang::tree_shake_functions(p, {}); }
            catch (const std::exception& e) { denied = std::string(e.what()).find("unresolved actor callback") != std::string::npos; }
            require(denied, "unresolved actor callback admitted"); ++checks;
        }
        {
            auto p = oreslang::parse_program(std::string(helpers) + actor);
            bool denied = false;
            try { oreslang::tree_shake_functions(p, {"helper"}); }
            catch (const std::exception& e) { denied = std::string(e.what()).find("ambiguous tree-shake root") != std::string::npos; }
            require(denied, "ambiguous explicit function root admitted"); ++checks;
        }
        {
            auto p = oreslang::parse_program("fnc recur(int n): int { return recur(n); } fnc unused(): int { return 0; } pub fnc main(): int { return recur(1); }");
            oreslang::tree_shake_functions(p, {});
            require(p.functions.size() == 2 && contains(p, "recur", 1), "recursive closure not retained"); ++checks;
        }
        std::printf("native actor AOT reachability: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
