#include "oreslang/frontend/GpuLaunchSourceMir.hpp"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
using namespace oreslang::gpu::source_mir;
int main() {
    int cases = 0;
    const auto positive = [] (bool logical) {
        return Candidate{"saxpy", {"alpha", "x", "y", "out", "n"}, "device", "queue",
                         {logical ? Mode::LogicalOverGroup : Mode::PhysicalGridBlock,
                          {I64::constant(logical ? 0 : 4), I64::constant(1), I64::constant(1)},
                          {I64::constant(256), I64::constant(1), I64::constant(1)}},
                          I64::constant(0)};
    };
    auto a = source_candidate(positive(false));
    assert(a.kernel == "saxpy" && a.ordered_arguments.size() == 5);
    assert(!a.executable() && !a.type_checked()); ++cases;
    auto logical = source_candidate(positive(true));
    assert(logical.geometry.mode == Mode::LogicalOverGroup); ++cases;
    auto ceil = I64::ceil_div(I64::constant(257), I64::constant(256));
    assert(ceil.kind == I64::Kind::Constant && ceil.value == 2); ++cases;
    auto big = I64::ceil_div(I64::constant(INT64_MAX), I64::constant(256));
    assert(big.value == INT64_MAX / 256 + (INT64_MAX % 256 != 0)); ++cases;
    auto sym = I64::ceil_div(I64::named("n"), I64::constant(256));
    assert(sym.kind == I64::Kind::CeilDiv && sym.left && sym.right); ++cases;
    const auto rejects = [&cases] (auto fn, const char* code) {
        bool rejected = false;
        try { fn(); } catch (const std::invalid_argument& e) {
            rejected = std::string(e.what()).find(code) != std::string::npos;
        }
        assert(rejected); ++cases;
    };
    rejects([] { (void)I64::ceil_div(I64::constant(3), I64::constant(0)); }, "GPU-LAUNCH-003");
    rejects([] { (void)I64::constant(-1); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.extent[0] = I64::constant(0); (void)source_candidate(std::move(x)); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(true); x.geometry.group[0] = I64::constant(0); (void)source_candidate(std::move(x)); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.extent.pop_back(); (void)source_candidate(std::move(x)); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.device.clear(); (void)source_candidate(std::move(x)); }, "GPU-LAUNCH-001");
    rejects([&] { auto x = positive(false); x.ordered_arguments.resize(65, "name"); (void)source_candidate(std::move(x)); }, "GPU-LAUNCH-002");
    std::cout << "PASS " << cases << " source MIR host-only checks (never executable)\n";
}
