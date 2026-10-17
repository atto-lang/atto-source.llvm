#include "oreslang/backend/BpfXdpLowering.hpp"

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

oreslang::Program good_program(std::int64_t action = 2) {
    oreslang::Program program;
    oreslang::Function fn;
    fn.name = "ores_xdp";
    fn.exported = true;
    fn.result = std::make_unique<oreslang::Expression>();
    fn.result->kind = oreslang::Expression::Kind::Integer;
    fn.result->integer = action;
    program.functions.push_back(std::move(fn));
    return program;
}

void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

template<class Mutate>
void rejects(const char* label, Mutate mutate) {
    auto program = good_program();
    mutate(program, program.functions.front());
    try {
        (void)oreslang::emit_verified_bpf_xdp_ir(program);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error(std::string("BPF backend admitted forbidden AST: ") + label);
}

} // namespace

int main() {
    for (std::int64_t action = 0; action <= 3; ++action) {
        auto program = good_program(action);
        const auto first = oreslang::emit_verified_bpf_xdp_ir(program);
        const auto second = oreslang::emit_verified_bpf_xdp_ir(program);
        check(first == second, "BPF backend output is not deterministic");
        check(first.find("target triple = \"bpfel\"") != std::string::npos,
              "BPF backend target triple missing");
        check(first.find("section \"xdp\"") != std::string::npos,
              "BPF backend XDP section missing");
        check(first.find("section \"license\"") != std::string::npos,
              "BPF backend license section missing");
        check(first.find("ret i32 " + std::to_string(action)) != std::string::npos,
              "BPF backend action mismatch");
        check(first.find("Proprietary") != std::string::npos,
              "BPF backend conservative license marker missing");
    }

    rejects("module", [](auto& p, auto&) { p.module = "m"; });
    rejects("class", [](auto& p, auto&) { p.classes.emplace_back(); });
    rejects("actor", [](auto& p, auto&) { p.actors.emplace_back(); });
    rejects("cooperative loop", [](auto& p, auto&) { p.cooperative_loops.emplace_back(); });
    rejects("second function", [](auto& p, auto&) { p.functions.emplace_back(); });
    rejects("wrong name", [](auto&, auto& f) { f.name = "main"; });
    rejects("private entry", [](auto&, auto& f) { f.exported = false; });
    rejects("parameter", [](auto&, auto& f) { f.parameters.emplace_back(); });
    rejects("generic", [](auto&, auto& f) { f.generic_parameters.emplace_back(); });
    rejects("void return", [](auto&, auto& f) { f.returns_void = true; });
    rejects("missing result", [](auto&, auto& f) { f.result.reset(); });
    rejects("nonliteral result", [](auto&, auto& f) {
        f.result->kind = oreslang::Expression::Kind::Call;
        f.result->name = "helper";
    });
    rejects("local", [](auto&, auto& f) { f.locals.emplace_back(); });
    rejects("statement", [](auto&, auto& f) { f.statements.emplace_back(); });
    rejects("guarded return", [](auto&, auto& f) { f.early_returns.emplace_back(); });
    rejects("actor spawn", [](auto&, auto& f) { f.actor_spawns.emplace_back(); });
    rejects("actor send", [](auto&, auto& f) { f.actor_sends.emplace_back(); });
    rejects("actor stop", [](auto&, auto& f) { f.actor_stops.emplace_back("a"); });
    rejects("actor action", [](auto&, auto& f) { f.actor_actions.emplace_back(); });
    rejects("actor field write", [](auto&, auto& f) { f.actor_field_writes.emplace_back(); });
    rejects("scalar loop", [](auto&, auto& f) {
        f.scalar_while = std::make_unique<oreslang::ScalarWhileLoop>();
    });
    rejects("caught throw", [](auto&, auto& f) {
        f.caught_throw_value = std::make_unique<oreslang::Expression>();
    });
    rejects("catch binding", [](auto&, auto& f) { f.catch_binding = "err"; });
    rejects("caught throw defer", [](auto&, auto& f) { f.caught_throw_defers.emplace_back(); });
    rejects("declared effects flag", [](auto&, auto& f) { f.has_declared_effects = true; });
    rejects("latent effect bits", [](auto&, auto& f) {
        f.declared_effects.bits =
            static_cast<std::uint32_t>(oreslang::FunctionEffect::Io);
    });
    rejects("forged effect bits", [](auto&, auto& f) {
        f.declared_effects.bits = 0x80000000u;
    });
    rejects("negative action", [](auto&, auto& f) { f.result->integer = -1; });
    rejects("redirect action", [](auto&, auto& f) { f.result->integer = 4; });
    rejects("large action", [](auto&, auto& f) { f.result->integer = 0x7fffffff; });

    std::cout << "BPF XDP backend direct-AST fail-closed matrix PASS\n";
    return 0;
}
