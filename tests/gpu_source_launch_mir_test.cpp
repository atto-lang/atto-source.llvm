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

    // Direct aggregate construction bypasses I64 factories; every candidate
    // must validate the full tree before it can enter another compiler pass.
    rejects([&] { auto x = positive(false); x.geometry.extent[0] =
            I64{I64::Kind::Constant, -1, {}, {}, {}}; (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.group[0] =
            I64{I64::Kind::Symbol, 0, "", {}, {}}; (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.extent[0] =
            I64{I64::Kind::Symbol, 0, "bad..symbol", {}, {}}; (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.extent[0] =
            I64{I64::Kind::Symbol, 2, "n", {}, {}}; (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.group[0] =
            I64{I64::Kind::CeilDiv, 0, {}, {}, {}}; (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.extent[0] =
            I64{I64::Kind::CeilDiv, 0, {},
                std::make_shared<I64>(I64::named("n")),
                std::make_shared<I64>(I64::constant(0))};
                (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.group[0] =
            I64{static_cast<I64::Kind>(99), 0, {}, {}, {}};
                (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.geometry.mode = static_cast<Mode>(99);
                (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.shared_bytes =
            I64{I64::Kind::Constant, -512, {}, {}, {}}; (void)source_candidate(x); }, "GPU-LAUNCH-003");
    rejects([&] { auto x = positive(false); x.ordered_arguments[0] = "alpha;host()";
                (void)source_candidate(x); }, "GPU-LAUNCH-001");
    rejects([&] { auto x = positive(false); x.kernel = ".invalid";
                (void)source_candidate(x); }, "GPU-LAUNCH-001");
    rejects([] { (void)I64::ceil_div(
                I64{I64::Kind::Constant, -1, {}, {}, {}}, I64::constant(2)); },
            "GPU-LAUNCH-003");
    rejects([] { (void)I64::ceil_div(
                I64::constant(3), I64{I64::Kind::Constant, -1, {}, {}, {}}); },
            "GPU-LAUNCH-003");
    rejects([&] {
        auto x = positive(false);
        auto deep = I64::named("n");
        for (int i = 0; i < 17; ++i)
            deep = I64::ceil_div(std::move(deep), I64::constant(2));
        x.geometry.extent[0] = std::move(deep);
        (void)source_candidate(x);
    }, "GPU-LAUNCH-003");
    rejects([&] {
        auto x = positive(false);
        auto huge = I64::named("n");
        for (int i = 0; i < 9; ++i)
            huge = I64::ceil_div(huge, huge);
        x.geometry.extent[0] = std::move(huge);
        (void)source_candidate(x);
    }, "GPU-LAUNCH-003");
    {
        auto cycle = std::make_shared<I64>(
            I64{I64::Kind::CeilDiv, 0, {}, {}, {}});
        cycle->left = cycle;
        cycle->right = std::make_shared<I64>(I64::constant(2));
        rejects([&] {
            auto x = positive(false);
            x.geometry.extent[0] = *cycle;
            (void)source_candidate(x);
        }, "GPU-LAUNCH-003");
        cycle->left.reset(); // explicitly break the test's deliberate cycle
    }
    rejects([] { (void)I64::named("bad..name"); }, "GPU-LAUNCH-003");
    rejects([] { (void)I64::ceil_div(
                I64{I64::Kind::Constant, 4, "forged", {}, {}},
                I64::constant(2)); }, "GPU-LAUNCH-003");
    rejects([] { (void)I64::ceil_div(
                I64::constant(4),
                I64{I64::Kind::Constant, 2, "forged", {}, {}}); },
            "GPU-LAUNCH-003");
    rejects([] { (void)I64::ceil_div(
                I64{I64::Kind::Constant, 4, {},
                    std::make_shared<I64>(I64::constant(9)), {}},
                I64::constant(2)); }, "GPU-LAUNCH-003");
    auto qualified = positive(false);
    qualified.kernel = "math.saxpy";
    qualified.queue = "device.queue";
    assert(source_candidate(qualified).kernel == "math.saxpy"); ++cases;
    std::cout << "PASS " << cases << " source MIR host-only checks (never executable)\n";
}
