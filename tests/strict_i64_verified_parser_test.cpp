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
    reject(main_source("42"),"E_VERIFY_UNSUPPORTED:");
    reject(pre+"type Narrow = i32 where self > 0; "+main_source("42"),"E_VERIFY_UNSUPPORTED:");
    std::cout << "PASS strict i64 real parser and validators; no native guest claim\n";
}
