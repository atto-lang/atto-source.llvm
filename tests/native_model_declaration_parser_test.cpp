#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ModelGraphAdmission.hpp"
#include <stdexcept>
#include <string>
#include <string_view>
#include <functional>
#include <iostream>

static void check(bool condition, const std::string& label) {
    if (!condition) throw std::runtime_error("native model parser test: " + label);
}
static void rejected(std::string_view source, std::string_view diagnostic) {
    try {
        (void)oreslang::parse_model_declarations(source);
    } catch (const std::exception& e) {
        check(std::string_view(e.what()).find(diagnostic) != std::string_view::npos,
              "wrong diagnostic: " + std::string(e.what()));
        return;
    }
    throw std::runtime_error("model parser unexpectedly accepted: " + std::string(source));
}
int main() {
    try {
        const auto ml = oreslang::parse_model_declarations(
            "define model Classifier(features: int = 784, hidden: int = 512) as "
            "Dense(features, hidden) ReLU() Dense(hidden, 10) end");
        check(ml.size() == 1, "ML declaration count");
        check(ml[0].name == "Classifier", "ML nominal type");
        check(ml[0].formals.size() == 2, "ML formals");
        check(ml[0].formals[0].default_value == "784", "default metadata");
        check(ml[0].operations.size() == 3, "ML ordered graph");
        check(ml[0].operations[0].arguments[0].formal_reference, "symbolic arg");
        const auto non_ml = oreslang::parse_model_declarations(
            "define module app as define model Tax(rate: float = 0.15) as "
            "Validate() Apply(rate) Round() end end");
        check(non_ml.size() == 1 && non_ml[0].operations.size() == 3, "non-ML module");
        check(non_ml[0].formals[0].default_value == "0.15", "float default");
        // Compiler-owned signature example: dollar-denominated scalar graph.
        const oreslang::model::Port usd{"f64", {}, "USD"};
        const oreslang::model::Port mass{"f64", {}, "kg"};
        const std::string digest(64, '0');
        const oreslang::model::Registry trusted = {
            {"Validate", {"Validate", usd, usd, "pure", "v1", digest, "verified-ores", 0, false}},
            {"Apply", {"Apply", usd, usd, "pure", "v1", digest, "verified-ores", 0, false}},
            {"Round", {"Round", usd, usd, "pure", "v1", digest, "verified-ores", 0, false}}
        };
        auto hir = oreslang::model::admit(non_ml[0], trusted,
                                        oreslang::model::Limits{64, 64, 4096});
        check(hir.name == "Tax" && hir.nodes.size() == 3, "typed HIR");
        check(hir.input.has_value() && *hir.input == usd && hir.output == usd,
              "nominal typed USD ports");
        check(hir.upper_bound_bytes == 32, "conservative buffer budget");
        check(!hir.nodes[0].vjp, "forward-only graph has no gradient requirement");
        const auto descriptor = oreslang::model::canonical_descriptor(hir, "app::Tax@v1");
        check(descriptor.size() == 514, "canonical descriptor byte length");
        check(oreslang::model::graph_digest(hir, "app::Tax@v1") ==
                  "7ffbb2b813a560a93cda79dfcdcd999183b65b1bfe2562002e85f5a7ba0b3545",
              "portable graph digest golden");
        check(oreslang::model::graph_digest(hir, "app::Tax@v1") !=
                  oreslang::model::graph_digest(hir, "other::Tax@v1"),
              "nominal model type must affect graph digest");

        const auto cfg_a = oreslang::parse_model_declarations(
            "define model X as Apply(1) end");
        const auto cfg_b = oreslang::parse_model_declarations(
            "define model X as Apply(2) end");
        const oreslang::model::Registry cfg_registry = {
            {"Apply", {"Apply", usd, usd, "pure", "v1", digest, "verified-ores", 0, false}}
        };
        const auto hir_a = oreslang::model::admit(cfg_a[0], cfg_registry);
        const auto hir_b = oreslang::model::admit(cfg_b[0], cfg_registry);
        check(hir_a.nodes[0].config != hir_b.nodes[0].config,
              "literal op configuration must affect HIR identity");

        const auto def_a = oreslang::parse_model_declarations(
            "define model X(rate: float = 0.15) as Apply(rate) end");
        const auto def_b = oreslang::parse_model_declarations(
            "define model X(rate: float = 0.20) as Apply(rate) end");
        const auto hdef_a = oreslang::model::admit(def_a[0], cfg_registry);
        const auto hdef_b = oreslang::model::admit(def_b[0], cfg_registry);
        check(hdef_a.formal_config != hdef_b.formal_config,
              "formal defaults must affect graph identity");
        auto bad = trusted;
        bad.at("Apply").input = mass;
        try {
            (void)oreslang::model::admit(non_ml[0], bad);
            throw std::runtime_error("unsound implicit unit conversion");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_PORT_TYPE_MISMATCH") != std::string_view::npos,
                  "wrong unit mismatch diagnostic: " + std::string(e.what()));
        }
        bad = trusted;
        bad.at("Apply").impl_digest = "bad";
        try {
            (void)oreslang::model::admit(non_ml[0], bad);
            throw std::runtime_error("unsound implementation digest");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_INVALID_IMPL_DIGEST") != std::string_view::npos,
                  "wrong implementation digest diagnostic: " + std::string(e.what()));
        }
        bad = trusted;
        bad.at("Apply").provenance = "guest-asserted";
        try {
            (void)oreslang::model::admit(non_ml[0], bad);
            throw std::runtime_error("unsound operation provenance");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_INVALID_OP_PROVENANCE") != std::string_view::npos,
                  "wrong provenance diagnostic: " + std::string(e.what()));
        }
        bad = trusted;
        bad.at("Apply").effect = "io";
        try {
            (void)oreslang::model::admit(non_ml[0], bad);
            throw std::runtime_error("unsound effectful op");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_EFFECTFUL_OP") != std::string_view::npos,
                  "wrong effect diagnostic: " + std::string(e.what()));
        }
        try {
            (void)oreslang::model::admit(non_ml[0], trusted,
                                        oreslang::model::Limits{64, 64, 31});
            throw std::runtime_error("unsound allocation bound");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_MEMORY_BUDGET") != std::string_view::npos,
                  "wrong memory diagnostic: " + std::string(e.what()));
        }
        check(oreslang::parse_model_declarations("fnc model(): int { return 42; }").empty(),
              "contextual model function name");
        rejected("define model Bad as end", "MODEL_EMPTY_BODY");
        rejected("define model Bad(x: int, x: int) as Op(x) end", "MODEL_DUPLICATE_FORMAL");
        rejected("define model Bad as Op(x) end", "MODEL_UNKNOWN_FORMAL");
        rejected("define model Bad as Op(foo: 1, foo: 2) end", "MODEL_DUPLICATE_ARGUMENT");
        rejected("define model Bad as Op(a: 1, 2) end", "MODEL_POSITIONAL_AFTER_NAMED");
        rejected("define model Bad as Op(1 + 2) end", "expected ')'");
        rejected("define model Bad as Op(foo()) end", "MODEL_UNKNOWN_FORMAL");
        rejected("define model Bad(x: int = z) as Op(x) end", "MODEL_DYNAMIC_ARGUMENT");
        std::string many = "define model Many as ";
        for (int i = 0; i < 65; ++i) many += "Op() ";
        many += "end";
        rejected(many, "MODEL_NODE_BUDGET");
        try {
            (void)oreslang::parse_program(
                "define model Tax(rate: float = 0.15) as Apply(rate) end");
            throw std::runtime_error("native compiler silently admitted unlowered model");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_LOWERING_UNSUPPORTED") != std::string_view::npos,
                  "native typed lowering must fail closed: " + std::string(e.what()));
        }
        std::cout << "PASS native model parser positive, negative and unimplemented lowering boundaries\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
