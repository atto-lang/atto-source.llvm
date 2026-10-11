#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/PipelineSourceLowering.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

static void check(bool result, const char* msg) {
    if (!result) throw std::runtime_error(msg);
}
template<typename Fn>
static void reject(Fn fn, const char* msg) {
    try { fn(); } catch (const std::exception&) { return; }
    throw std::runtime_error(msg);
}
static void replace_once(std::string& source,
                         const std::string& from, const std::string& to) {
    const auto at = source.find(from);
    if (at == std::string::npos) throw std::runtime_error("fixture changed unexpectedly");
    source.replace(at, from.size(), to);
}
int main(int argc, char** argv) {
    if (argc != 2) throw std::runtime_error("expected source .ores path");
    std::ifstream file(argv[1]);
    if (!file) throw std::runtime_error("cannot read source .ores");
    const std::string source{std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>()};
    const auto layout = oreslang::dop::Plan(oreslang::dop::Layout::AoSoA, 7, 3, 4, 16, 4);
    const auto target = oreslang::dop::GpuTarget{64, 4096, 100, true};
    const auto plan = oreslang::compile_pipeline_source(source, layout, target);
    check(plan.entry == "do_work", "wrong compiler entry");
    check(plan.stages[0].input == "void" && plan.stages[0].output == "f32",
          "producer port mismatch");
    check(plan.stages[1].input == "f32" && plan.stages[1].output == "f32",
          "gpu stage port mismatch");
    check(plan.stages[2].input == "f32" && plan.stages[2].output == "Results",
          "collector port mismatch");
    check(plan.kernel.required_bytes == layout.byte_length(), "GPU geometry mismatch");
    check(plan.kernel.device_source.find("__kernel void oreslang_dop_f32") != std::string::npos,
          "compiler did not emit real GPU IR");
    reject([&] { (void)oreslang::parse_program(source); },
           "ordinary guest compiler bypassed accelerator and actor effects gate");

    auto mismatch = source;
    replace_once(mismatch, "StreamOperator<f32, f32>", "StreamOperator<void, f32>");
    reject([&] { (void)oreslang::compile_pipeline_source(mismatch, layout, target); },
           "invalid actor stream ports admitted");

    auto wrong_graph = source;
    replace_once(wrong_graph, "pipe spawn GpuWorker()", "pipe spawn CollectResultsWorker()");
    reject([&] { (void)oreslang::compile_pipeline_source(wrong_graph, layout, target); },
           "wrong actor order admitted");

    auto side_effect = source;
    replace_once(side_effect, "return value * 2.0 + 1.0;",
                 "return value * 2.0 + await value;");
    reject([&] { (void)oreslang::compile_pipeline_source(side_effect, layout, target); },
           "GPU host effect admitted");

    auto actor_body = source;
    replace_once(actor_body, "StreamOperator<void, f32> as\nend",
                 "StreamOperator<void, f32> as\n  on_start(): void { return; }\nend");
    reject([&] { (void)oreslang::compile_pipeline_source(actor_body, layout, target); },
           "unimplemented actor source method was admitted");

    auto duplicate = source;
    replace_once(duplicate, "define actor GpuWorker",
                 "define actor CpuWorker");
    reject([&] { (void)oreslang::compile_pipeline_source(duplicate, layout, target); },
           "duplicate actor definition admitted");

    auto extra = source + "pub routine unsafe(): void { return; }";
    reject([&] { (void)oreslang::compile_pipeline_source(extra, layout, target); },
           "unexpected source declarations admitted");

    auto empty = source;
    replace_once(empty, "spawn GpuWorker()", "spawn GpuWorker(1)");
    reject([&] { (void)oreslang::compile_pipeline_source(empty, layout, target); },
           "constructor args bypassed actor no-constructor invariant");

    // A large source cannot amplify compile memory or tokenization CPU.
    reject([&] { (void)oreslang::compile_pipeline_source(
            std::string(65537, 'x'), layout, target); },
            "oversized source was admitted");
}
