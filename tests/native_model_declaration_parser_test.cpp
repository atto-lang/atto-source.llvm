#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ModelGraphAdmission.hpp"
#include "oreslang/frontend/ModelRunFencing.hpp"
#include "oreslang/frontend/PortableModelGraphDigestV1.hpp"
#include "oreslang/frontend/ResolvedModelGraphLoweringV1.hpp"
#include "oreslang/frontend/ExecutionProfileDigestV1.hpp"
#include <stdexcept>
#include <string>
#include <string_view>
#include <functional>
#include <utility>
#include <iostream>
#include <limits>
namespace pv1 = oreslang::model::portable_v1;
namespace rv1 = oreslang::model::resolved_v1;
namespace epv1 = oreslang::model::profile_v1;

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
        const auto min_i64 = oreslang::parse_model_declarations(
            "define model Minimum(delta: int = -9223372036854775808) as "
            "AddDelta(delta) end");
        check(min_i64[0].formals[0].default_value == "-9223372036854775808",
              "signed INT64_MIN default must remain exact");
        const auto neg_i64 = oreslang::parse_model_declarations(
            "define model Negative as AddDelta(-2) end");
        check(neg_i64[0].operations[0].arguments[0].value == "-2",
              "signed static literal must remain exact");
        rejected("define model Bad as AddDelta(-(1 + 1)) end",
                 "MODEL_NONCONSTANT_CONFIG");
        rejected("define model Bad as AddDelta(-true) end",
                 "MODEL_NONCONSTANT_CONFIG");
        rejected("define model Bad as AddDelta(-2.5) end",
                 "MODEL_NONCONSTANT_CONFIG");
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
        // The parser is not the security boundary. Directly constructed
        // AST values must meet identical static-value and resource limits.
        auto deny_forged_ast = [&](oreslang::ModelDecl forged, std::string_view expected) {
            try {
                (void)oreslang::model::admit(forged, trusted);
                throw std::runtime_error("forged native model AST unexpectedly admitted");
            } catch (const std::exception& e) {
                check(std::string_view(e.what()) == expected,
                      "wrong direct-AST admission diagnostic");
            }
        };
        auto malformed_float = non_ml[0];
        malformed_float.formals[0].default_value = "0.15junk";
        deny_forged_ast(malformed_float, "MODEL_INVALID_CONST");

        auto integer_overflow = ml[0];
        integer_overflow.formals[0].default_value = "9223372036854775808";
        deny_forged_ast(integer_overflow, "MODEL_CONST_OVERFLOW");

        auto too_many_arguments = non_ml[0];
        too_many_arguments.operations[1].arguments.resize(17);
        deny_forged_ast(too_many_arguments, "MODEL_ARGUMENT_BUDGET");

        auto unresolved_named = non_ml[0];
        unresolved_named.operations[1].arguments[0].label = "rate";
        deny_forged_ast(unresolved_named, "MODEL_NAMED_ARGUMENT_UNRESOLVED");

        auto forged_name = non_ml[0];
        forged_name.name = "bad model name";
        deny_forged_ast(forged_name, "MODEL_NONCONSTANT_CONFIG");

        auto forged_op = non_ml[0];
        forged_op.operations.front().name = "Bad.Op";
        deny_forged_ast(forged_op, "MODEL_INVALID_OP_ID");

        auto forged_many = non_ml[0];
        while (forged_many.operations.size() < 65)
            forged_many.operations.push_back(forged_many.operations.front());
        try {
            (void)oreslang::model::admit(forged_many, trusted,
                                         oreslang::model::Limits{1000, 1000, 100000});
            throw std::runtime_error("native AST bypassed hard v1 node limit");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()) == "MODEL_NODE_BUDGET",
                  "wrong inflated-limits diagnostic");
        }

        auto malformed_argument = non_ml[0];
        malformed_argument.operations[1].arguments[0].formal_reference = false;
        malformed_argument.operations[1].arguments[0].value = "3bogus";
        deny_forged_ast(malformed_argument, "MODEL_INVALID_CONST");

        const pv1::NominalPort price{"finance.Price@v1", 16};
        const pv1::Profile pp{true, "none", true};
        auto portable_sig = [&](char digest_char, std::vector<std::string> kinds,
                                std::vector<pv1::Port> inputs) {
            return pv1::Signature{std::move(inputs), price, "pure", false, 0,
                "ores.model.op.v1", std::string(64, digest_char), "verified-ores",
                std::move(kinds), pp};
        };
        pv1::Graph portable{1, "finance.PriceEstimate@v1", {
            {"source", "finance.PriceInput", {}, {},
                portable_sig('1', {}, {})},
            {"tax", "finance.ApplyTaxBps", {"source"}, {{"i64", "1500"}},
                portable_sig('2', {"i64"}, {price})},
            {"round", "finance.RoundCents", {"tax"}, {},
                portable_sig('3', {}, {price})}
        }, {"round"}};
        check(pv1::canonical_bytes(portable).size() == 544, "portable digest byte length");
        const auto graph_id = pv1::digest(portable);
        check(graph_id == "e4b3337bd3e8b71d8f062afc7fcad5646fa8ad567bfe009e3d02f599b3ee833c",
              "shared portable graph digest");
        auto constant_change = portable;
        constant_change.nodes[1].const_args[0].value = "1501";
        check(pv1::digest(constant_change) ==
                  "1dcfd1569621a30e53c969b9bf0e2add7fe94a04c2bf4474fae821555996d21b",
              "static constant digest mutation");
        auto implementation_change = portable;
        implementation_change.nodes[1].signature.impl_digest = std::string(64, '4');
        check(pv1::digest(implementation_change) ==
                  "cdb96a21464b7fbeeaa3c7fade033780785c7849bf15a6033634b060b597e5ce",
              "implementation digest mutation");
        auto renamed = portable;
        renamed.nodes[0].id = "a";
        renamed.nodes[1].id = "b"; renamed.nodes[1].inputs = {"a"};
        renamed.nodes[2].id = "c"; renamed.nodes[2].inputs = {"b"};
        renamed.outputs = {"c"};
        check(pv1::digest(renamed) == graph_id, "temporary node IDs are nonsemantic");

        epv1::Profile reference_profile{
            1, "reference", "model-ref-1", "ores-semantic-1",
            "portable", "portable", "core-model-v1", "strict", true,
            "preallocated", true, std::nullopt, {}
        };
        check(epv1::canonical_bytes(reference_profile).size() == 127,
              "execution profile canonical byte length");
        const auto derived_profile = epv1::digest(reference_profile);
        check(derived_profile ==
                  "8440c7b53174d2d0dc6a6ff6e1fda662e409f4e21f6ce647eea38b87b3ff9695",
              "shared execution profile digest");
        auto ieee_profile = reference_profile;
        ieee_profile.numeric_mode = "ieee";
        check(epv1::digest(ieee_profile) ==
                  "f157ad0ec51a93c6f89e12512af934acea2d179f07c577aa0399a51dffc4275e",
              "execution numeric mode mutation");
        auto feature_profile = reference_profile;
        feature_profile.numeric_features = {"fma"};
        check(epv1::digest(feature_profile) ==
                  "dad02ababd944847f5fac379b1c9e8f924233df789a46f436b70e48f3881d55b",
              "execution feature mutation");
        auto target_profile = reference_profile;
        target_profile.target_triple = "x86_64-unknown-linux-gnu";
        check(epv1::digest(target_profile) ==
                  "50d472c73b925bb180cada7a0def1f4d386f58a09bbdcd547221ed67bbc5a7e1",
              "execution target mutation");

        const rv1::Registry resolved_registry = {
            {"PriceInput", portable_sig('1', {}, {})},
            {"ApplyTaxBps", portable_sig('2', {"i64"}, {price})},
            {"RoundCents", portable_sig('3', {}, {price})}
        };
        const auto resolved_literal_source = oreslang::parse_model_declarations(
            "define model PriceEstimate as PriceInput() ApplyTaxBps(1500) RoundCents() end");
        const auto resolved_literal = rv1::lower(
            resolved_literal_source[0], "finance.PriceEstimate@v1", resolved_registry);
        check(pv1::digest(resolved_literal) == graph_id,
              "parsed literal source lowers to shared graph digest");

        auto wrong_typed_registry = resolved_registry;
        wrong_typed_registry.at("ApplyTaxBps").inputs = {
            pv1::NominalPort{"finance.Weight@v1", 16}
        };
        try {
            (void)rv1::lower(resolved_literal_source[0],
                             "finance.PriceEstimate@v1", wrong_typed_registry);
            throw std::runtime_error("resolved lowering released ill-typed model graph");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()) == "MODEL_PORT_TYPE_MISMATCH",
                  "typed predecessor mismatch must fail inside lowering");
        }

        const auto resolved_default_source = oreslang::parse_model_declarations(
            "define model PriceEstimate(tax_bps: int = 1500) as "
            "PriceInput() ApplyTaxBps(tax_bps) RoundCents() end");
        auto check_lowering_failure = [&](oreslang::ModelDecl source,
                                         const rv1::Overrides& values,
                                         std::string_view expected) {
            try {
                (void)rv1::lower(source, "finance.PriceEstimate@v1", resolved_registry, values);
                throw std::runtime_error("invalid direct model AST escaped resolved lowering");
            } catch (const std::exception& e) {
                check(std::string_view(e.what()) == expected,
                      "resolved lowering failed with incorrect model diagnostic");
            }
        };
        const auto unused_source = oreslang::parse_model_declarations(
            "define model PriceEstimate(unused: int = 1) as "
            "PriceInput() ApplyTaxBps(1500) RoundCents() end");
        auto malformed_unused = unused_source[0];
        malformed_unused.formals[0].default_value = "1garbage";
        check_lowering_failure(malformed_unused, {}, "MODEL_INVALID_CONST");
        check_lowering_failure(unused_source[0], {{"unused", "2garbage"}},
                               "MODEL_INVALID_CONST");
        auto too_many_nodes = unused_source[0];
        while (too_many_nodes.operations.size() < 65)
            too_many_nodes.operations.push_back(too_many_nodes.operations.front());
        check_lowering_failure(too_many_nodes, {}, "MODEL_NODE_BUDGET");
        auto too_many_formals = unused_source[0];
        while (too_many_formals.formals.size() < 17)
            too_many_formals.formals.push_back(too_many_formals.formals.front());
        check_lowering_failure(too_many_formals, {}, "MODEL_FORMAL_BUDGET");

        const auto resolved_default = rv1::lower(
            resolved_default_source[0], "finance.PriceEstimate@v1", resolved_registry);
        check(pv1::digest(resolved_default) == graph_id,
              "model default resolves before graph hashing");
        const auto resolved_override = rv1::lower(
            resolved_default_source[0], "finance.PriceEstimate@v1", resolved_registry,
            {{"tax_bps", "1501"}});
        check(pv1::digest(resolved_override) ==
                  "1dcfd1569621a30e53c969b9bf0e2add7fe94a04c2bf4474fae821555996d21b",
              "model override resolves before graph hashing");

        const auto labelled_source = oreslang::parse_model_declarations(
            "define model PriceEstimate as PriceInput() ApplyTaxBps(tax_bps: 1500) RoundCents() end");
        try {
            (void)rv1::lower(labelled_source[0], "finance.PriceEstimate@v1", resolved_registry);
            throw std::runtime_error("named configuration was silently erased");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_NAMED_ARGUMENT_UNRESOLVED")
                      != std::string_view::npos, "wrong unresolved named config diagnostic");
        }

        auto duplicate_formal = resolved_default_source[0];
        duplicate_formal.formals.push_back(duplicate_formal.formals.front());
        try {
            (void)rv1::lower(duplicate_formal, "finance.PriceEstimate@v1", resolved_registry);
            throw std::runtime_error("forged duplicate formal admitted");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_DUPLICATE_FORMAL")
                      != std::string_view::npos, "wrong duplicate formal diagnostic");
        }

        const rv1::Registry external_registry = {
            {"ValidatePrice", portable_sig('1', {}, {price})},
            {"ApplyTaxBps", portable_sig('2', {"i64"}, {price})},
            {"RoundCents", portable_sig('3', {}, {price})}
        };
        const auto external_source = oreslang::parse_model_declarations(
            "define model PriceTransform as ValidatePrice() ApplyTaxBps(1500) RoundCents() end");
        const auto external_graph = rv1::lower(
            external_source[0], "finance.PriceTransform@v1", external_registry);
        check(pv1::canonical_bytes(external_graph).size() == 577,
              "external input graph byte length");
        check(pv1::digest(external_graph) ==
                  "069288245a672efe264581df42bfec4686192fdb210dca8ce7da7dda563ecac3",
              "external run input graph digest");
        check(external_graph.nodes[0].signature.inputs.size() == 1 &&
                  external_graph.nodes[0].inputs.empty(),
              "first signature input is external run boundary");

        const std::string profile = derived_profile;
        const oreslang::model::ModelOwner owner{
            "finance.PriceEstimate@v1", graph_id, 7, "instance-1", 11, profile
        };
        const oreslang::model::RunStamp run{
            "run-1", "finance.PriceEstimate@v1", graph_id, 7, "instance-1", 11, profile
        };
        auto ledger = oreslang::model::validate_and_commit(owner, run, {});
        check(ledger.committed_run_ids.count("run-1") == 1, "run stamp commit");
        try {
            (void)oreslang::model::validate_and_commit(owner, run, ledger);
            throw std::runtime_error("duplicate run unexpectedly committed");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_RUN_REPLAY") != std::string_view::npos,
                  "wrong duplicate run diagnostic");
        }
        auto stale = run;
        stale.parameter_version = 12;
        try {
            (void)oreslang::model::validate_and_commit(owner, stale, {});
            throw std::runtime_error("stale parameter version unexpectedly committed");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_PARAMETER_VERSION_MISMATCH") != std::string_view::npos,
                  "wrong stale parameter diagnostic");
        }
        check(oreslang::model::next_graph_generation(7) == 8, "graph generation increment");
        check(oreslang::model::next_parameter_version(11) == 12, "parameter version increment");
        check(oreslang::model::plan_cache_key(graph_id, profile) !=
                  oreslang::model::plan_cache_key(graph_id, std::string(64, 'c')),
              "execution profile participates in compiled plan cache identity");

        const oreslang::model::ModelOwner stateless_owner{
            "finance.PriceEstimate@v1", graph_id, 1, "instance-1", std::nullopt, profile
        };
        const oreslang::model::RunStamp stateless_run{
            "run-stateless", "finance.PriceEstimate@v1", graph_id, 1,
            "instance-1", std::nullopt, profile
        };
        auto stateless_ledger = oreslang::model::validate_and_commit(
            stateless_owner, stateless_run, {});
        check(stateless_ledger.committed_run_ids.count("run-stateless") == 1,
              "stateless run stamp commit");
        auto wrongly_stateful = stateless_run;
        wrongly_stateful.run_id = "run-stateful";
        wrongly_stateful.parameter_version = 0;
        try {
            (void)oreslang::model::validate_and_commit(
                stateless_owner, wrongly_stateful, {});
            throw std::runtime_error("stateless parameter mismatch unexpectedly committed");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_PARAMETER_VERSION_MISMATCH") != std::string_view::npos,
                  "wrong stateless parameter mismatch diagnostic");
        }
        try {
            oreslang::model::ModelOwner bad_id{
                " bad id ", graph_id, 0, "instance-1", std::nullopt, profile
            };
            oreslang::model::validate_owner(bad_id);
            throw std::runtime_error("invalid model type id unexpectedly accepted");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_INVALID_MODEL_TYPE_ID") != std::string_view::npos,
                  "wrong model type id diagnostic");
        }
        auto expect_fence = [&](oreslang::model::RunStamp candidate, std::string_view code) {
            try {
                (void)oreslang::model::validate_and_commit(owner, candidate, {});
                throw std::runtime_error("model fence unexpectedly accepted");
            } catch (const std::exception& e) {
                check(std::string_view(e.what()).find(code) != std::string_view::npos,
                      "wrong model fence diagnostic");
            }
        };
        auto wrong_graph = run; wrong_graph.graph_digest = std::string(64, 'c');
        expect_fence(wrong_graph, "MODEL_INSTANCE_GRAPH_MISMATCH");
        auto wrong_generation = run; wrong_generation.graph_generation = 8;
        expect_fence(wrong_generation, "MODEL_GRAPH_GENERATION_MISMATCH");
        auto wrong_instance = run; wrong_instance.instance_id = "instance-2";
        expect_fence(wrong_instance, "MODEL_INSTANCE_ID_MISMATCH");
        auto wrong_profile = run; wrong_profile.execution_profile_digest = std::string(64, 'd');
        expect_fence(wrong_profile, "MODEL_EXECUTION_PROFILE_MISMATCH");
        try {
            (void)oreslang::model::next_graph_generation(std::numeric_limits<std::int64_t>::max());
            throw std::runtime_error("graph generation wrapped");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_GRAPH_GENERATION_OVERFLOW") != std::string_view::npos,
                  "wrong graph generation overflow diagnostic");
        }
        try {
            (void)oreslang::model::next_parameter_version(std::numeric_limits<std::int64_t>::max());
            throw std::runtime_error("parameter version wrapped");
        } catch (const std::exception& e) {
            check(std::string_view(e.what()).find("MODEL_PARAMETER_VERSION_OVERFLOW") != std::string_view::npos,
                  "wrong parameter version overflow diagnostic");
        }

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
            check(std::string_view(e.what()).find("MODEL_LOWERING_NOT_IMPLEMENTED") != std::string_view::npos,
                  "native typed lowering must fail closed: " + std::string(e.what()));
        }
        std::cout << "PASS native model parser positive, negative and unimplemented lowering boundaries\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
