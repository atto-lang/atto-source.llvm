#include "oreslang/frontend/GpuLaunchSourceMir.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang::gpu::source_mir;

static int checks = 0;
static void expect(bool result, const char* context) {
    ++checks;
    if (!result) throw std::runtime_error(std::string("GPU test failed: ") + context);
}
template <typename F>
static void expect_rejected(F&& fn, const char* diagnostic) {
    bool seen = false;
    try { fn(); }
    catch (const std::invalid_argument& ex) {
        seen = std::string(ex.what()).find(diagnostic) != std::string::npos;
    }
    expect(seen, diagnostic);
}
static Candidate valid() {
    return {"math.saxpy", {"alpha", "view_x", "view_y", "out", "n"},
            "gpu_device", "compute.queue", {Mode::LogicalOverGroup,
             {I64::named("n"), I64::constant(1), I64::constant(1)},
             {I64::constant(256), I64::constant(1), I64::constant(1)}},
            I64::constant(0)};
}
static std::string valid_name(std::mt19937_64& rng) {
    const std::vector<std::string> segments={"x", "a1", "_", "GPU_2", "queue", "abc_xyz"};
    std::string result=segments[rng()%segments.size()];
    for (unsigned i=0, count=static_cast<unsigned>(rng()%3);i<count;++i)
        result += "." + segments[rng()%segments.size()];
    return result;
}
int main() {
    // No default constructor or untyped source MIR gives an execution permit.
    const auto base = source_candidate(valid());
    expect(!base.executable() && !base.type_checked(), "source candidate must remain nonexecutable");

    // Lexical names must be ASCII identifier segments. NUL, Unicode and
    // embedded punctuation are invalid; qualified names are allowed.
    for (const auto& s : std::vector<std::string>{"", ".x", "x.", "a..b", "a-b",
              "0a", "a.0b", "a b", "abc()", "n;host()", std::string("a\0b", 3),
              std::string("\xc3\xa9", 2), "a/b"}) {
        expect(!source_symbol(s), "bad identifier must be rejected");
        auto req = valid(); req.kernel=s;
        expect_rejected([&] {(void)source_candidate(req);}, "GPU-LAUNCH-001");
        expect_rejected([&] {(void)I64::named(s);}, "GPU-LAUNCH-003");
    }
    for (const auto& s: std::vector<std::string>{"a", "_", "a0", "ns.foo", "_a._b123"})
        expect(source_symbol(s), "well formed symbol should pass");
    expect(!source_symbol(std::string(257,'x')), "oversized identifier");

    // Property/mutation test: construct a valid launch and mutate exactly one
    // field. Independently classify each case rather than comparing two calls
    // to the same verifier. The seed makes failures reproducible.
    std::mt19937_64 rng{0xA770BEEF5EEDULL};
    constexpr int randomized = 8192;
    for (int i=0;i<randomized;++i) {
        auto x=valid();
        x.kernel=valid_name(rng);
        x.device=valid_name(rng);
        x.queue=valid_name(rng);
        x.geometry.mode=(rng()%2) ? Mode::PhysicalGridBlock : Mode::LogicalOverGroup;
        x.geometry.extent[0]=I64::constant(static_cast<std::int64_t>(rng()%1000)+(x.geometry.mode==Mode::PhysicalGridBlock));
        x.geometry.group[0]=I64::constant(static_cast<std::int64_t>(rng()%1024)+1);
        const unsigned mutation=static_cast<unsigned>(rng()%16);
        bool reject=false;
        const char* diagnostic="GPU-LAUNCH-003";
        switch (mutation) {
            case 0: break; // valid
            case 1: x.kernel=valid_name(rng); break; // valid
            case 2: x.geometry.extent[1]=I64::ceil_div(I64::named("width"),I64::constant(2)); break; // valid
            case 3: x.shared_bytes=I64::ceil_div(I64::named("shared"),I64::constant(64)); break; // valid
            case 4: x.geometry.extent[0]=I64{I64::Kind::Constant,-1,{},{},{}}; reject=true; break;
            case 5: x.geometry.group[1]=I64::constant(0); reject=true; break;
            case 6: x.geometry.mode=static_cast<Mode>(99); reject=true; break;
            case 7: x.geometry.extent.pop_back(); reject=true; break;
            case 8: x.geometry.group[1]=I64{I64::Kind::Symbol,1,"i",{},{}}; reject=true; break;
            case 9: x.shared_bytes=I64{I64::Kind::CeilDiv,0,{},{},{}}; reject=true; break;
            case 10: x.geometry.extent[1]=I64{I64::Kind::CeilDiv,0,{},
                         std::make_shared<I64>(I64::named("n")),std::make_shared<I64>(I64::constant(0))}; reject=true; break;
            case 11: x.geometry.group[0]=I64{I64::Kind::Constant,4,"evil",{},{}}; reject=true; break;
            case 12: x.geometry.extent[1]=I64{static_cast<I64::Kind>(255),0,{},{},{}}; reject=true; break;
            case 13: x.queue="bad..queue"; diagnostic="GPU-LAUNCH-001"; reject=true; break;
            case 14: x.ordered_arguments.resize(65,"a"); diagnostic="GPU-LAUNCH-002"; reject=true; break;
            case 15: x.geometry.extent[0]=I64::constant(0);
                     reject=x.geometry.mode==Mode::PhysicalGridBlock; break;
        }
        if (reject) expect_rejected([&] {(void)source_candidate(x);},diagnostic);
        else {
            const auto candidate=source_candidate(x);
            expect(!candidate.executable() && !candidate.type_checked(),"valid mutated launch non-executable");
        }
    }

    // Shared noncyclic DAGs should be admitted within the structural budget.
    // A tree requiring >256 visits is rejected even if the graph is small in
    // unique pointer count. There is no exponential expensive validation.
    {
        auto small=I64::named("n");
        for(int i=0;i<6;++i) small=I64::ceil_div(small,small);
        auto x=valid();x.geometry.extent[0]=std::move(small);
        expect(!source_candidate(x).executable(),"within-budget repeated tree");
    }
    {
        auto oversized=I64::named("n");
        for(int i=0;i<8;++i) oversized=I64::ceil_div(oversized,oversized);
        auto x=valid();x.geometry.extent[0]=std::move(oversized);
        expect_rejected([&]{(void)source_candidate(x);},"GPU-LAUNCH-003");
    }

    // A cycle has no execution or compiler-visible evaluation side effect.
    auto cyclic=std::make_shared<I64>(I64::ceil_div(I64::named("n"),I64::constant(2)));
    cyclic->left=cyclic;
    auto cycle_req=valid();cycle_req.geometry.extent[0]=*cyclic;
    expect_rejected([&]{(void)source_candidate(cycle_req);},"GPU-LAUNCH-003");
    cyclic->left.reset();
    cycle_req.geometry.extent[0].left.reset();
    std::cout << "PASS " << checks << " host-only adversarial source-MIR assertions, "
              << randomized << " deterministic differential mutations; gpu_executed=false\n";
}
