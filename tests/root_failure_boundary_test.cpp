#include "oreslang/runtime/RootFailureBoundary.hpp"
#include <stdexcept>
#include <initializer_list>
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("failed: " #condition); } while (false)
using namespace oreslang;
int main() {
    const auto ok = root_terminal_outcome({EffectKind::Return, 42u});
    CHECK(!ok.failed && ok.default_exit_status == 0);
    CHECK(ok.terminal.kind == EffectKind::Return && ok.original.code == 42u);

    const auto ordinary = root_terminal_outcome({EffectKind::Throw, 11u});
    CHECK(ordinary.failed && ordinary.default_exit_status == 1);
    CHECK(ordinary.terminal.kind == EffectKind::Panic);
    CHECK(ordinary.original.kind == EffectKind::Throw && ordinary.original.code == 11u);

    const auto raised = root_terminal_outcome({EffectKind::Raise, 12u});
    CHECK(raised.failed && raised.default_exit_status == 1);
    CHECK(raised.terminal.kind == EffectKind::Panic);
    CHECK(raised.original.kind == EffectKind::Raise && raised.original.code == 12u);

    for (auto kind : {EffectKind::Panic, EffectKind::Cancel, EffectKind::Fatal}) {
        const auto outcome = root_terminal_outcome({kind, 99u});
        CHECK(outcome.failed && outcome.default_exit_status == 1);
        CHECK(outcome.terminal.kind == kind && outcome.original.kind == kind);
    }

    const auto forged = root_terminal_outcome({static_cast<EffectKind>(0xff), 7u});
    CHECK(forged.failed && forged.default_exit_status == 1);
    CHECK(forged.terminal.kind == EffectKind::Fatal);
    CHECK(forged.original.code == 7u);
}
