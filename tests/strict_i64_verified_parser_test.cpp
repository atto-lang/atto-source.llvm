#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/VerifiedI64Range.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

int main() {
    using namespace oreslang;
    const std::string pre = "define dimension Time; define unit Second: Time; type Bounded = int where self >= 1 and self <= 42; ";
    auto main_source = [](std::string_view e) {
        return "pub fnc main() -> int { return " + std::string(e) + "; }";
    };
    auto reject = [](std::string_view source, std::string_view code) {
        try { (void)parse_verified_strict_i64(source); }
        catch (const std::exception& e) {
            if (std::string_view(e.what()).find(code) != std::string_view::npos) return;
            throw;
        }
        throw std::runtime_error("strict source unexpectedly admitted");
    };
    for (auto body : {main_source("40+2"),main_source("84 / 2"),main_source("42 as Bounded"),
            "fnc f(Bounded x) -> int { return x+1; } "+main_source("f(41)"),
            main_source("-9223372036854775808")}) {
        auto program=parse_verified_strict_i64(pre+body);
        if(program.functions.empty()) throw std::runtime_error("missing lowered function");
    }
    for (auto body : {main_source("42/0"),main_source("-9223372036854775808 / -1"),
            main_source("9223372036854775807+1"),main_source("(9223372036854775807+1)*0"),
            "fnc f(int x) -> int { return -x; } "+main_source("f(-42)"),
            "fnc f(int x) -> int { return x+1; } "+main_source("f(41)"),
            "fnc f(int x) -> Bounded { return x as Bounded; } "+main_source("f(42)"),
            std::string("pub fnc main() -> int { val int x = 9223372036854775807+1; return 42; }")})
        reject(pre+body,"E_VERIFY_UNPROVEN:");
    const std::string header="fnc f(int x) -> int requires x >= 1 requires x <= 42";
    for(std::string_view body:{" { return 84/x; }","\nbegin return 84/x; end"," :: return 84/x; end","\n:: return 84/x; end"})
        (void)parse_verified_strict_i64(pre+header+std::string(body)+" "+main_source("f(2)"));
    reject(pre+header+" begin return 84/x; end "+main_source("f(2)"),"E_SYNTAX:");
    reject(main_source("42"),"E_VERIFY_UNSUPPORTED:");
    reject(pre+"type Narrow = i32 where self > 0; "+main_source("42"),"E_VERIFY_UNSUPPORTED:");
    for(std::string_view source:{"fnc f(int x) -> int { if x != 0 { return 42/x; } return 0; } pub fnc main() -> int { return f(1); }",
            "fnc f(int x) -> int { if x != 0 && 42/x > 0 { return 1; } return 42; } pub fnc main() -> int { return f(0); }",
            "fnc f(int x) -> int { if x == 0 || 42/x > 0 { return 42; } return 0; } pub fnc main() -> int { return f(0); }",
            "fnc f(int x) -> int { if x != 0 && x != -1 { return -9223372036854775808/x; } return 0; } pub fnc main() -> int { return f(2); }",
            "fnc f(int x) -> int { if !(x == 0) { return 42/x; } else { return 42; } } pub fnc main() -> int { return f(0); }",
            "fnc f(int x) -> int { if x < 0 || x > 0 { return 42/x; } return 0; } pub fnc main() -> int { return f(-1); }"}) {
        auto program=parse_verified_strict_i64(pre+std::string(source));
        if(program.functions.empty())throw std::runtime_error("missing lowered conditional");
    }
    for(std::string_view source:{"fnc f(int x) -> int { if 42/x > 0 && x != 0 { return 42; } return 0; } pub fnc main() -> int { return f(1); }",
            "fnc f(int x) -> int { if x != 0 { return -9223372036854775808/x; } return 0; } pub fnc main() -> int { return f(2); }",
            "fnc f(int x) -> int { if x != 0 || 42/x > 0 { return 42; } return 0; } pub fnc main() -> int { return f(0); }",
            "fnc f(int x, int y) -> int { if x != 0 { return 42/y; } return 0; } pub fnc main() -> int { return f(1,1); }",
            "fnc f(int x) -> int { if x != 0 || x == 0 { return 42/x; } return 0; } pub fnc main() -> int { return f(1); }"})reject(pre+std::string(source),"E_VERIFY_UNPROVEN:");
    for (std::string_view body : {
        "type Small = int where self >= 1 and self <= 21; fnc f(Small n) -> Bounded { val a=n+n; return a; } pub fnc main() -> int { return f(21); }",
        "pub fnc main() -> int { let int x = 1; x = 42; return x; }",
        "pub fnc main() -> int { let Bounded x = 1; while x < 42 { x = x+1; } return x; }",
        "pub fnc main() -> int { let int x = 42; while x > 0 { x = x-1; } return x+42; }",
        "pub fnc main() -> int { let int x = 0; while x < 21 { x = x+1; } while x < 42 { x = x+1; } return x; }"
    }) (void)parse_verified_strict_i64(pre+std::string(body));
    reject(pre+"pub fnc main() -> int { let int x = 1; x = 0; return 42/x; }","E_VERIFY_UNPROVEN:");
    reject(pre+"pub fnc main() -> int { let int x = 0; while x >= 0 { x = x+1; } return x; }","E_VERIFY_UNPROVEN:");
    reject(pre+"pub fnc main() -> int { let Bounded x = 1; while x <= 42 { x = x+1; } return x; }","E_REFINEMENT_BINDING:");
    reject(pre+"pub fnc main() -> int { let int x = 1; while x > 0 { x = x-1; } return 42/x; }","E_VERIFY_UNPROVEN:");
    reject(pre+"pub fnc main() -> int { val a=1; a=0; return a; }","E_VERIFY_UNSUPPORTED:");
    reject(pre+"pub fnc main() -> int { val a=a; return 42; }","E_REFINEMENT_NAME:");
    reject(pre+"pub fnc main() -> int { val a=9223372036854775807+1; return 42; }","E_VERIFY_UNPROVEN:");
    reject(pre+"pub fnc main() -> int { let int x=0; while x < 42 { x=x+1; x=x+1; } return x; }","E_VERIFY_UNSUPPORTED:");
    std::cout << "PASS strict i64 real parser and validators; no native guest claim\n";
}
