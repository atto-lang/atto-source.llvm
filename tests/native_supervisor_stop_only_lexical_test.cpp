#include "oreslang/frontend/NativeSupervisorStopOnly.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using oreslang::NativeSupervisorStopOnlyPlan;
using oreslang::compile_native_stop_only_supervisor;

static_assert(!std::is_default_constructible_v<NativeSupervisorStopOnlyPlan>,
              "guest source must not mint a trusted STOP policy");

int main() {
    int admitted = 0;
    int denied = 0;
    const auto allow = [&](std::string_view source) {
        try {
            (void)compile_native_stop_only_supervisor(source);
            ++admitted;
        } catch (const std::exception& ex) {
            throw std::runtime_error("unexpected supervisor source denial: " +
                                     std::string(ex.what()));
        }
    };
    const auto reject = [&](std::string_view source) {
        try { (void)compile_native_stop_only_supervisor(source); }
        catch (const std::invalid_argument&) { ++denied; return; }
        throw std::runtime_error("unsafe supervisor source admitted");
    };

    const std::string canonical =
        "define supervisor S as on_panic(f: Failure): void { self.stop(f); return; } end";
    allow(canonical);
    allow("define pub supervisor S as on_panic(f: Failure): void { self.stop(); return; } end");
    allow("pub define supervisor S as @Implements pub on_panic(f: Failure): void "
          "{ self.stop(f); return; } end");
    allow("define supervisor S as on_panic(f: Failure) -> void "
          "{ self.stop(f); return; } end");
    allow("/*define actor malicious as*/ " + canonical);
    allow("define supervisor S as on_panic(f: Failure): void "
          "{ self/*comment*/.stop(f); return; } end");
    allow(canonical + " // ignored");
    allow(canonical + " /* outer /* nested */ outer */");

    for (const std::string& invalid : std::vector<std::string>{
        "define supervisor gpu as on_panic(f: Failure): void { self.stop(f); return; } end",
        "define supervisor S as on_panic(rt: Failure): void { self.stop(rt); return; } end",
        "define supervisor actor as on_panic(f: Failure): void { self.stop(f); return; } end",
        "define supervisor class as on_panic(f: Failure): void { self.stop(f); return; } end",
        "define supervisor S as on_panic(match: Failure): void { self.stop(match); return; } end",
        "define supervisor S as on_panic(actor: Failure): void { self.stop(actor); return; } end",
        "define supervisor S as on_panic(f: String): void { self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): int { self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(other); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f.child); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f.actor); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f); self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.restart(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void { return; self.stop(f); } end",
        canonical + " define actor Extra as end",
        "define module Work as " + canonical + " end",
        "define supervisor S as async on_panic(f: Failure): void "
          "{ await self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void "
          "{ await self.stop(f); return; } end",
        canonical + " /* unterminated",
        canonical + " \"forged\"",
        "pub define pub supervisor S as on_panic(f: Failure): void "
          "{ self.stop(f); return; } end",
        "define supervisor S as on_panic(self: Failure): void "
          "{ self.stop(self); return; } end",
        canonical + " #ready",
        canonical + " try {}",
        canonical + " @Implements",
        canonical + " async",
        canonical + " \xC3\xA9",
        canonical + " \xF0\x9F\x90\xB1",
        canonical + std::string(1, '\0'),
        canonical + " " + std::string(129, 'A'),
        "define supervisor " + std::string(129, 'A') + " as on_panic(f: Failure): void "
          "{ self.stop(f); return; } end",
        canonical + std::string(200, ';'),
        "/*" + std::string(100000, 'x') + canonical,
        canonical + " /*" + std::string(40, 'x'),
        std::string(1048577, ' '),
        canonical + " // comment" + std::string(1, '\0')
    }) reject(invalid);

    if (admitted != 8 || denied != 36)
        throw std::runtime_error("incomplete source admission test enumeration");
    std::cout << "NATIVE_STOP_SOURCE_LEXICAL=PASS accepted=" << admitted
              << " denied=" << denied << '\n';
}
