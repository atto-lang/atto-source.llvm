#include "oreslang/types/NominalMethodSlotPolicy.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace oreslang::types::overloads;
namespace m = oreslang::types::overloads::methods;

static void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
template<class F> static void denied(F operation, const char* why) {
    bool rejected = false;
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, why);
}

int main() {
    const std::unordered_set<std::string> ids{"root::Color", "root::Mode"};
    const m::Slot color{m::Domain::Instance,
        signature("choose", {{Kind::Enum, "root::Color"}}, ids)};
    const m::Slot mode{m::Domain::Instance,
        signature("choose", {{Kind::Enum, "root::Mode"}}, ids)};
    const m::Slot staticColor{m::Domain::Static, color.signature};
    const m::Slot staticMode{m::Domain::Static, mode.signature};
    const auto a = m::proveLocalSameArityFamilies(
        {color, mode, staticMode, staticColor}, ids);
    const auto b = m::proveLocalSameArityFamilies(
        {staticColor, mode, color, staticMode}, ids);
    check(a.size() == 4 && b.size() == 4, "static/instance signatures lost");
    for (std::size_t i = 0; i < a.size(); ++i) {
        check(m::linkSymbol("root.Picker", a[i]) ==
              m::linkSymbol("root.Picker", b[i]), "source-order dependent slots");
    }
    // Must equal Java NominalMethodSlots.Slot.linkSymbol byte for byte.
    check(m::linkSymbol("root.Picker", color) ==
        "$methodoverload$v1$11_726f6f742e5069636b6572$"
        "8_494e5354414e4345$6_63686f6f7365$1$"
        "4_454e554d$11_726f6f743a3a436f6c6f72$", "Java ABI mismatch");
    check(m::linkSymbol("root.Picker", color) !=
          m::linkSymbol("root.Picker", staticColor), "domain collision");
    check(m::linkSymbol("root.Picker", color) !=
          m::linkSymbol("root.Other", color), "owner collision");

    const m::Slot primitive{m::Domain::Instance,
        signature("choose", {{Kind::Primitive, "int"}}, ids)};
    denied([&] { (void)m::proveLocalSameArityFamilies({primitive, color}, ids); },
           "primitive vs enum admitted");
    denied([&] { (void)m::proveLocalSameArityFamilies({color, color}, ids); },
           "duplicate signature admitted");
    denied([&] { (void)m::proveLocalSameArityFamilies({color, mode},
                                                     {"root::Color"}); },
           "uncertified enum admitted");
    const m::Slot wide{m::Domain::Instance, Signature{"choose",
        std::vector<Position>(MaxOverloadArity + 1, {Kind::Enum, "root::Color"})}};
    denied([&] { (void)m::linkSymbol("root.Picker", wide); },
           "public aggregate escaped arity budget");
    denied([&] { (void)m::proveLocalSameArityFamilies(
        std::vector<m::Slot>(MaxSameArityCandidates + 1, color), ids); },
           "same-arity candidate budget escaped");
    denied([&] { (void)m::linkSymbol("root.Picker",
        {static_cast<m::Domain>(255), color.signature}); }, "invalid domain");
    denied([&] { (void)m::linkSymbol(
        std::string(1, static_cast<char>(0xff)), color); }, "bad UTF-8");
    denied([&] { (void)m::linkSymbol("root.Picker",
        {m::Domain::Instance, Signature{"choose",
         {{Kind::Enum, "root::0Bad"}}}}); }, "forged enum identity");
    check(m::proveLocalSameArityFamilies({color, staticColor}, ids).empty(),
          "single-slot domains incorrectly treated as typed overloads");
    denied([&] { (void)m::proveLocalSameArityFamilies(
        std::vector<m::Slot>(m::MaxLocalMethodDeclarations + 1, color), ids); },
        "large class must reject before candidate-family allocation");
    const m::Slot longName{m::Domain::Instance,
        Signature{std::string(m::MaxMethodNameBytes + 1, 'a'),
            {{Kind::Enum, "root::Color"}}}};
    denied([&] { (void)m::linkSymbol("root.Picker", longName); },
        "overlong method name bypassed link ABI ceiling");
    denied([&] { (void)m::proveLocalSameArityFamilies(
        {longName, longName}, ids); },
        "overlong method name bypassed proof admission");
    denied([&] { (void)m::linkSymbol(
        std::string(m::MaxMethodOwnerBytes + 1, 'a'), color); },
        "overlong nominal class identity bypassed link ABI ceiling");
    const std::string utf8Name(128, 'a');
    check(m::linkSymbol("root.Picker", {m::Domain::Instance,
        Signature{utf8Name, {{Kind::Enum, "root::Color"}}}}).size() > 0,
        "valid bounded method identifier rejected");

    std::string multibyteName;
    for (std::size_t i = 0; i < 128; ++i) multibyteName += "\xc3\xa9";
    check(m::linkSymbol("root.Picker", {m::Domain::Instance,
        Signature{multibyteName, {{Kind::Enum, "root::Color"}}}}).size() > 0,
        "256-byte UTF-8 method name incorrectly rejected");
    multibyteName += "\xc3\xa9";
    denied([&] { (void)m::linkSymbol("root.Picker",
        {m::Domain::Instance,
         Signature{multibyteName, {{Kind::Enum, "root::Color"}}}}); },
         "258-byte UTF-8 method name bypassed ceiling");
    denied([&] { (void)m::linkSymbol("root.Picker", {
        m::Domain::Instance,
        Signature{std::string("choose\nevil"), {{Kind::Enum, "root::Color"}}}}); },
        "control newline in method symbol accepted");
    denied([&] { (void)m::linkSymbol(std::string("root\0Pick", 9), color); },
        "NUL control byte in owner symbol accepted");
    denied([&] { (void)m::proveLocalSameArityFamilies({
        {m::Domain::Instance,
         Signature{std::string("get\0evil", 8), {{Kind::Enum, "root::Color"}}}},
        {m::Domain::Instance,
         Signature{std::string("get\0evil", 8), {{Kind::Enum, "root::Mode"}}}}
    }, ids); }, "embedded NUL method name admitted by compiler proof");
    std::cout << "nominal method slot policy OK\n";
    return 0;
}
