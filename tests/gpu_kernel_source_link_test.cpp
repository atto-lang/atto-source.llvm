#include "oreslang/frontend/GpuKernelSourceLink.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::gpu;
using source_link::Parameter;
using source_link::Signature;

static int verified = 0;
static void check(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
    ++verified;
}
template<class F> static void denied(F fn, const char* prefix) {
    bool rejected = false;
    try { fn(); }
    catch (const std::invalid_argument& e) {
        rejected = std::string(e.what()).rfind(prefix, 0) == 0;
    }
    check(rejected, prefix);
}
static Signature signature() {
    return {"saxpy", {{"alpha", typed_ir::Parameter::F32},
                      {"x", typed_ir::Parameter::ReadF32},
                      {"y", typed_ir::Parameter::ReadF32},
                      {"out", typed_ir::Parameter::WriteF32},
                      {"n", typed_ir::Parameter::U64}}, true, true};
}
static source_mir::Candidate launch() {
    return {"saxpy", {"alpha","x","y","out","n"}, "device", "queue",
             {source_mir::Mode::LogicalOverGroup,
              {source_mir::I64::named("n"),source_mir::I64::constant(1),source_mir::I64::constant(1)},
              {source_mir::I64::constant(256),source_mir::I64::constant(1),source_mir::I64::constant(1)}},
             source_mir::I64::constant(0)};
}
int main() {
    const auto typed = source_link::signature_to_typed_shape(signature());
    check(typed.symbol == "saxpy", "kernel symbol");
    check(typed.returns_void, "kernel return void");
    check(typed.parameters.size() == 5 && typed.parameters[0] == typed_ir::Parameter::F32
          && typed.parameters[1] == typed_ir::Parameter::ReadF32
          && typed.parameters[3] == typed_ir::Parameter::WriteF32
          && typed.parameters[4] == typed_ir::Parameter::U64, "typed parameter order");
    check(typed.effects.size() == 2 && typed.effects[0] == typed_ir::Effect::DeviceRead
          && typed.effects[1] == typed_ir::Effect::DeviceWrite, "effects shape");
    const auto linked = source_link::link({signature()}, launch());
    check(linked.unresolved_host_arguments.size() == 5 &&
          linked.unresolved_host_arguments.front() == "alpha" &&
          linked.unresolved_host_arguments.back() == "n", "ordered unresolved source args");
    check(!linked.executable() && !linked.host_arguments_typed()
          && !linked.kernel_body_verified(), "source link cannot promote execution");
    check(linked.kernel_shape.parameters.size() == 5, "typed signature linked");
    denied([] { (void)source_link::link({}, launch()); }, "GPU-LAUNCH-001");
    denied([] { auto c = launch(); c.kernel = "unknown"; (void)source_link::link({signature()}, c); }, "GPU-LAUNCH-001");
    denied([] { auto c = launch(); c.ordered_arguments.pop_back(); (void)source_link::link({signature()}, c); }, "GPU-LAUNCH-002");
    denied([] { auto c = launch(); c.ordered_arguments.emplace_back("extra"); (void)source_link::link({signature()}, c); }, "GPU-LAUNCH-002");
    denied([] { auto c = launch(); c.geometry.group[0] = source_mir::I64::constant(0); (void)source_link::link({signature()}, c); }, "GPU-LAUNCH-003");
    denied([] { auto s = signature(); s.parameters[1].name = "alpha"; (void)source_link::checked_signature(s); }, "GPU-LAUNCH-001");
    denied([] { auto s = signature(); s.parameters[1].name = "a.b"; (void)source_link::checked_signature(s); }, "GPU-LAUNCH-001");
    denied([] { auto s = signature(); s.parameters[0].type = static_cast<typed_ir::Parameter>(77); (void)source_link::checked_signature(s); }, "GPU-LAUNCH-002");
    denied([] { auto s = signature(); s.returns_void = false; (void)source_link::checked_signature(s); }, "GPU-LAUNCH-001");
    denied([] { auto s = signature(); s.source_body_scanned_only = false; (void)source_link::checked_signature(s); }, "GPU-LAUNCH-001");
    denied([] { auto s = signature(); s.parameters.resize(65, {"z",typed_ir::Parameter::F32}); (void)source_link::checked_signature(s); }, "GPU-LAUNCH-001");
    denied([] { (void)source_link::checked_catalog({signature(),signature()}); }, "GPU-LAUNCH-001");
    denied([] { auto bad = signature(); bad.symbol = ".not_a_name"; (void)source_link::checked_signature(bad); }, "GPU-LAUNCH-001");
    {
        auto s = signature(); s.symbol = "other";
        auto pair = source_link::checked_catalog({signature(), s});
        check(pair.size() == 2, "two distinct kernels");
    }
    // Signed int and unsigned u64 are distinct; no implicit numeric
    // conversion at the typed GPU-argument boundary.
    {
        auto sig = signature();
        sig.parameters.back().type = typed_ir::Parameter::I64;
        const auto link = source_link::link({sig}, launch());
        check(link.kernel_shape.parameters.back() == typed_ir::Parameter::I64,
              "signed int source signature preserved");
        const Limits caps{{65535,65535,65535}, {1024,1024,64},
                                    1024,49152};
        typed_ir::Request req{
            {"signed", {typed_ir::Parameter::I64}, {}, true},
            {typed_ir::ScalarI64{-7}},
            {"tenant", "device", "queue", 1},
            typed_ir::Physical{{1,1,1},{1,1,1}},
            0, caps, 1, 1
        };
        const auto valid = typed_ir::verify(req);
        check(valid.kernel_symbol == "signed" && !valid.executable(),
              "signed scalar host preflight remains non-executable");
        req.arguments = {typed_ir::ScalarU64{7}};
        denied([&] { (void)typed_ir::verify(req); }, "GPU-LAUNCH-002");
        req.kernel.parameters = {typed_ir::Parameter::U64};
        req.arguments = {typed_ir::ScalarI64{7}};
        denied([&] { (void)typed_ir::verify(req); }, "GPU-LAUNCH-002");
    }
    // N distinct parameters and declaration sizes, preserving order.
    for (int count=0;count<=64;++count) {
        Signature s{"kernel", {}, true, true};
        auto c = launch();
        c.kernel = "kernel";
        c.ordered_arguments.clear();
        for (int i=0;i<count;++i) {
            auto name = "a" + std::to_string(i);
            s.parameters.push_back({name, typed_ir::Parameter::U64});
            c.ordered_arguments.push_back(name);
        }
        const auto r = source_link::link({s}, c);
        check(r.kernel_shape.parameters.size() == static_cast<std::size_t>(count)
              && r.unresolved_host_arguments.size() == static_cast<std::size_t>(count)
              && !r.executable(), "arity property from 0 to 64");
    }
    // Effect summaries remain read/write-only and never include host effects.
    for (int mask=0;mask<16;++mask) {
        Signature s{"kernel", {}, true, true};
        for (int i=0;i<4;++i) s.parameters.push_back({
            "p" + std::to_string(i),
            (mask & (1<<i)) ? typed_ir::Parameter::WriteF32 : typed_ir::Parameter::ReadF32});
        auto shape = source_link::signature_to_typed_shape(s);
        for (auto e : shape.effects)
            check(e == typed_ir::Effect::DeviceRead || e == typed_ir::Effect::DeviceWrite,
                  "effect summary must not grant host effects");
    }
    std::cout << "PASS " << verified
              << " host-only GPU kernel signature and launch-source-link checks; executable=false\n";
}
