#include "oreslang/frontend/Parser.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

struct Case { const char* expression; const char* diagnostic; };
static std::string check(const std::string& source) {
    try { (void)oreslang::parse_program(source); return {}; }
    catch (const std::exception& e) { return e.what(); }
}
int main() {
    const std::string helpers =
        "fnc read(borrow int x): int { return x; }\n"
        "fnc inspect(borrow mut int x): int { return x; }\n"
        "fnc hold_read(borrow int x, int y): int { return x + y; }\n"
        "fnc hold_write(borrow mut int x, int y): int { return x + y; }\n"
        "fnc read_two(borrow int x, borrow int y): int { return x + y; }\n"
        "fnc write_two(borrow mut int x, borrow mut int y): int { return x + y; }\n"
        "fnc value_first(int x, borrow mut int y): int { return x + y; }\n";
    const std::vector<Case> cases = {
        {"n", ""}, {"(rt copy n) + n", ""}, {"(rt share n) + n", ""},
        {"(rt take n) + 1", ""},
        {"(rt take n) + n", "use of explicitly moved int"},
        {"(rt take n) + (rt take n)", "use of explicitly moved int"},
        {"read(rt borrow n)", ""}, {"inspect(rt borrow mut n)", ""},
        {"read_two(rt borrow n, rt borrow n)", ""},
        {"write_two(rt borrow mut n, rt borrow mut n)", "conflicting active native borrow"},
        {"hold_read(rt borrow n, inspect(rt borrow mut n))", "conflicting active native borrow"},
        {"hold_write(rt borrow mut n, read(rt borrow n))", "conflicting active native borrow"},
        {"hold_write(rt borrow mut n, n)", "read conflicts with active mutable borrow"},
        {"hold_read(rt borrow n, rt take n)", "cannot move int while borrow is active"},
        {"read(rt borrow (n + 1))", "addressable named int"},
        {"rt borrow n", "borrowed reference may only be passed"},
        {"inspect(rt borrow n)", "borrow mode does not match"},
        {"value_first(n, rt borrow mut n)", ""},
        {"value_first(rt take n, rt borrow mut n)", "borrow of explicitly moved int"},
        {"hold_read(y: rt take n, x: rt borrow n)", "borrow of explicitly moved int"},
        {"value_first(y: rt borrow mut n, x: n)", "read conflicts with active mutable borrow"},
        {"hold_read(x: rt borrow n, y: n)", ""},
    };
    unsigned accepted=0, rejected=0;
    for (const auto& item:cases) {
        const auto body=std::string("return ")+item.expression+";";
        const auto ordinary=check(helpers+"fnc wrapper(int n): int {"+body+"}");
        const auto actor=check(helpers+"define actor Worker as { let total: int = 0; pub fnc on_message(int n): int {"+body+"} }");
        if (std::string(item.diagnostic).empty()) {
            if (!ordinary.empty() || !actor.empty()) {
                std::fprintf(stderr,"valid parity case rejected: %s\nfunction: %s\nactor: %s\n",item.expression,ordinary.c_str(),actor.c_str()); return 1;
            }
            ++accepted;
        } else {
            if (ordinary.find(item.diagnostic)==std::string::npos || actor.find(item.diagnostic)==std::string::npos) {
                std::fprintf(stderr,"ownership parity failure: %s\nfunction: %s\nactor: %s\n",item.expression,ordinary.c_str(),actor.c_str()); return 1;
            }
            ++rejected;
        }
    }
    std::printf("function/actor ownership parity: %u valid + %u invalid expression pairs passed\n",accepted,rejected);
    return 0;
}
