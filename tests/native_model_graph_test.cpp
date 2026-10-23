#include "oreslang/frontend/ModelGraph.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <cassert>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#ifndef ORES_MODEL_CORPUS_PATH
#error "ORES_MODEL_CORPUS_PATH must point to the vendored shared model corpus"
#endif

struct SharedCase {
    std::string id;
    std::string expect;
    std::string java_mode;
    std::string llvm_mode;
    std::string diagnostic;
    std::string llvm_diagnostic;
    std::string graph_digest;
    std::string configured_digest;
    std::string semantic_result;
    std::string configuration_i64;
    std::string source;
};

struct SharedIdentity {
    std::string id;
    bool expect_same = false;
    std::string expected_graph_digest;
    std::string variant_operation_digest;
    std::string variant_graph_digest;
    std::string baseline_source;
    std::string variant_source;
};

struct SharedCorpus {
    std::string revision;
    std::string profile;
    std::vector<SharedCase> cases;
    std::vector<SharedIdentity> identities;
};

std::vector<std::string> split_fields(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto pos = line.find('|', start);
        if (pos == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
}

int hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    throw std::runtime_error("invalid shared corpus hex");
}

std::string decode_hex_ascii(const std::string& hex) {
    if ((hex.size() & 1U) != 0U)
        throw std::runtime_error("odd shared corpus hex length");
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int value = (hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1]);
        if (value > 0x7f) throw std::runtime_error("non-ASCII shared corpus source");
        out.push_back(static_cast<char>(value));
    }
    return out;
}

std::string dash_empty(const std::string& value) {
    return value == "-" ? std::string{} : value;
}

SharedCorpus load_shared_corpus() {
    std::ifstream input(ORES_MODEL_CORPUS_PATH);
    if (!input) throw std::runtime_error("cannot open vendored shared model corpus");

    std::string line;
    if (!std::getline(input, line) || line != "ORESLANG_MODEL_EXECUTABLE_CASES_V1")
        throw std::runtime_error("invalid shared model corpus header");

    SharedCorpus corpus;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto fields = split_fields(line);
        if (fields[0] == "revision") {
            assert(fields.size() == 2);
            corpus.revision = fields[1];
        } else if (fields[0] == "profile") {
            assert(fields.size() == 2);
            corpus.profile = fields[1];
        } else if (fields[0] == "case") {
            assert(fields.size() == 12);
            corpus.cases.push_back(SharedCase{
                fields[1], fields[2], fields[3], fields[4],
                dash_empty(fields[5]), dash_empty(fields[6]),
                dash_empty(fields[7]), dash_empty(fields[8]),
                dash_empty(fields[9]), dash_empty(fields[10]),
                decode_hex_ascii(fields[11])
            });
        } else if (fields[0] == "identity") {
            assert(fields.size() == 8);
            corpus.identities.push_back(SharedIdentity{
                fields[1], fields[2] == "true",
                dash_empty(fields[3]), dash_empty(fields[4]),
                dash_empty(fields[5]), decode_hex_ascii(fields[6]),
                decode_hex_ascii(fields[7])
            });
        } else {
            throw std::runtime_error("unknown shared model corpus record");
        }
    }
    return corpus;
}

constexpr const char* kSharedExecutableCorpusRevision =
    "model-core-i64-v1.2026-10-20.10";

void expect_error(const std::string& code, const std::string& source) {
    try {
        (void)oreslang::parse_program(source);
        assert(false && "expected native model admission failure");
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()).find(code) != std::string::npos);
    }
}

} // namespace

int main() {
    assert(std::string(kSharedExecutableCorpusRevision) ==
           "model-core-i64-v1.2026-10-20.10");
    assert(oreslang::model_sha256("abc") ==
           "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");

    const auto shared_corpus = load_shared_corpus();
    assert(shared_corpus.revision == kSharedExecutableCorpusRevision);
    assert(shared_corpus.profile == "model-core-i64-v1");
    assert(shared_corpus.cases.size() >= 15);
    for (const auto& item : shared_corpus.cases) {
        if (item.llvm_mode == "reject") {
            const std::string expected =
                item.llvm_diagnostic.empty() ? item.diagnostic : item.llvm_diagnostic;
            assert(!expected.empty());
            expect_error(expected, item.source);
            continue;
        }

        auto accepted = oreslang::parse_program(item.source);
        assert(!accepted.models.empty());
        assert(accepted.models.front().portable_profile == "model-core-i64-v1");
        if (!item.graph_digest.empty())
            assert(accepted.models.front().graph_digest == item.graph_digest);
        if (!item.configured_digest.empty()) {
            assert(!item.configuration_i64.empty());
            assert(!accepted.model_runs.empty());
            assert(accepted.model_runs.front().configured_graph_digest ==
                   item.configured_digest);
            assert(accepted.model_runs.front().configuration.size() == 1);
            assert(std::to_string(accepted.model_runs.front().configuration.front()) ==
                   item.configuration_i64);
        }
    }
    for (const auto& item : shared_corpus.identities) {
        auto baseline = oreslang::parse_program(item.baseline_source);
        auto variant = oreslang::parse_program(item.variant_source);
        assert(!baseline.models.empty() && !variant.models.empty());
        if (item.expect_same) {
            assert(baseline.models.front().graph_digest ==
                   variant.models.front().graph_digest);
            if (!item.expected_graph_digest.empty())
                assert(baseline.models.front().graph_digest ==
                       item.expected_graph_digest);
        } else {
            assert(baseline.models.front().graph_digest !=
                   variant.models.front().graph_digest);
            if (!item.variant_graph_digest.empty())
                assert(variant.models.front().graph_digest ==
                       item.variant_graph_digest);
        }
    }

    const std::string source = R"ores(
        fnc Identity(int input): int {
            return input;
        }

        fnc AddDelta(int delta, int input): int {
            return input + delta;
        }

        pub define model PriceAdjust(delta: int = 2) as
            Identity()
            AddDelta(delta)
        end

        pub fnc main(): int {
            return PriceAdjust().run(40);
        }
    )ores";

    auto program = oreslang::parse_program(source);
    assert(program.models.size() == 1);
    assert(program.models.front().exported);
    assert(program.model_runs.size() == 1);

    // Visibility spelling is syntactic sugar, not part of graph identity.
    std::string canonical_source = source;
    const std::string legacy_spelling = "pub define model PriceAdjust";
    const auto visibility_at = canonical_source.find(legacy_spelling);
    assert(visibility_at != std::string::npos);
    canonical_source.replace(visibility_at, std::string("pub define model").size(),
                             "define pub model");
    const auto canonical_program = oreslang::parse_program(canonical_source);
    assert(canonical_program.models.size() == 1);
    assert(canonical_program.models.front().exported);
    assert(canonical_program.models.front().graph_digest ==
           program.models.front().graph_digest);
    assert(canonical_program.model_runs.size() == 1);
    assert(canonical_program.model_runs.front().configured_graph_digest ==
           program.model_runs.front().configured_graph_digest);
    assert(program.models.front().name == "PriceAdjust");
    assert(program.models.front().graph_digest ==
           "6c5e66eb67149e59c7d4d2d962b5671850ee3298f729214127d1452cb8d5ca21");
    assert(program.model_runs.front().blueprint_graph_digest ==
           program.models.front().graph_digest);
    assert(program.model_runs.front().configured_graph_digest ==
           "3975a5571cb2fddcc6206ee0b3f0f893dcb3fa9bfbe591b8c5b9f5ab8ff539e1");
    assert(program.model_runs.front().configuration.size() == 1);
    assert(program.model_runs.front().configuration.front() == 2);

    const auto& main_fn = program.functions.back();
    assert(main_fn.name == "main");
    assert(main_fn.result);
    // Model.run is erased before ordinary ownership/codegen admission.
    assert(main_fn.result->kind == oreslang::Expression::Kind::Call);
    assert(main_fn.result->name == "AddDelta");
    assert(main_fn.result->arguments.size() == 2);
    assert(main_fn.result->arguments[0]->kind == oreslang::Expression::Kind::Integer);
    assert(main_fn.result->arguments[0]->integer == 2);
    assert(main_fn.result->arguments[1]->kind == oreslang::Expression::Kind::Call);
    assert(main_fn.result->arguments[1]->name == "Identity");

    assert(program.models.front().portable_profile == "model-core-i64-v1");

    const std::string alpha_renamed_source = R"ores(
        fnc Identity(int x): int {
            return x;
        }

        fnc AddDelta(int amount, int value): int {
            return value + amount;
        }

        pub define model PriceAdjust(delta: int = 2) as
            Identity()
            AddDelta(delta)
        end

        pub fnc main(): int {
            return PriceAdjust().run(40);
        }
    )ores";
    auto alpha_renamed = oreslang::parse_program(alpha_renamed_source);
    assert(alpha_renamed.models.size() == 1);
    assert(alpha_renamed.models.front().portable_profile == "model-core-i64-v1");
    assert(alpha_renamed.models.front().graph_digest ==
           program.models.front().graph_digest);

    const std::string operand_order_source = R"ores(
        fnc Identity(int input): int {
            return input;
        }

        fnc AddDelta(int delta, int input): int {
            return delta + input;
        }

        pub define model PriceAdjust(delta: int = 2) as
            Identity()
            AddDelta(delta)
        end

        pub fnc main(): int {
            return PriceAdjust().run(40);
        }
    )ores";
    auto operand_order = oreslang::parse_program(operand_order_source);
    assert(operand_order.models.size() == 1);
    assert(operand_order.models.front().graph_digest ==
           "f87d8d1742d0915134a88e54bb8fb66e6cd65f156a753c1c1f3570614ec0a6ba");
    assert(operand_order.models.front().graph_digest !=
           program.models.front().graph_digest);

    const std::string negative_source = R"ores(
        fnc Identity(int input): int {
            return input;
        }

        fnc AddDelta(int delta, int input): int {
            return input + delta;
        }

        pub define model PriceAdjust(delta: int = -2) as
            Identity()
            AddDelta(delta)
        end

        pub fnc main(): int {
            return PriceAdjust().run(40);
        }
    )ores";

    auto negative = oreslang::parse_program(negative_source);
    assert(negative.models.size() == 1);
    assert(negative.models.front().graph_digest ==
           "f5b7956076ea5bc9696f4cd23d68c3e4c67d7ec90023a64c834d14d304249c1d");
    assert(negative.model_runs.size() == 1);
    assert(negative.model_runs.front().configured_graph_digest ==
           "15ca34acbdc5478bc2971ae7895d708aca2d324be418819a2e60b3c04bc20a5c");
    assert(negative.model_runs.front().configuration.size() == 1);
    assert(negative.model_runs.front().configuration.front() == -2);

    const std::string min_source = R"ores(
        fnc Identity(int input): int {
            return input;
        }

        fnc AddDelta(int delta, int input): int {
            return input + delta;
        }

        pub define model PriceAdjust(delta: int = -9223372036854775808) as
            Identity()
            AddDelta(delta)
        end

        pub fnc main(): int {
            return 0;
        }
    )ores";
    auto min_program = oreslang::parse_program(min_source);
    assert(min_program.models.size() == 1);
    assert(min_program.models.front().graph_digest ==
           "cb008cb043de50f1663ba8faa77de94c50d92514fa2b95cbd72de5225cf5cb80");

    const std::string min_run_source = R"ores(
        fnc Identity(int input): int { return input; }
        fnc AddDelta(int delta, int input): int { return input + delta; }
        pub define model PriceAdjust(delta: int = -9223372036854775808) as
            Identity()
            AddDelta(delta)
        end
        pub fnc main(): int { return PriceAdjust().run(0); }
    )ores";
    auto min_run = oreslang::parse_program(min_run_source);
    assert(min_run.model_runs.size() == 1);
    assert(min_run.model_runs.front().configured_graph_digest ==
           "27ed4d26451fa960a8271495ee8ac96bbb67bc3369ca45cf2205222a962de5a3");
    assert(min_run.model_runs.front().configuration.size() == 1);
    assert(min_run.model_runs.front().configuration.front() ==
           std::numeric_limits<std::int64_t>::min());

    const std::string negative_literal_source = R"ores(
        fnc AddDelta(int delta, int input): int {
            return input + delta;
        }
        define model Fixed as
            AddDelta(-2)
        end
        pub fnc main(): int {
            return Fixed().run(5);
        }
    )ores";
    auto negative_literal = oreslang::parse_program(negative_literal_source);
    assert(negative_literal.model_runs.size() == 1);

    expect_error("MODEL_DUPLICATE_PARAMETER", R"ores(
        fnc Identity(int input): int { return input; }
        define model Bad(delta: int, delta: int) as
            Identity()
        end
    )ores");

    expect_error("MODEL_UNKNOWN_FORMAL", R"ores(
        fnc AddDelta(int delta, int input): int { return input + delta; }
        define model Bad as
            AddDelta(missing)
        end
    )ores");

    expect_error("MODEL_CONST_OVERFLOW", R"ores(
        fnc AddDelta(int delta, int input): int { return input + delta; }
        define model Bad(delta: int = 9223372036854775808) as
            AddDelta(delta)
        end
    )ores");
    expect_error("MODEL_CONST_OVERFLOW", R"ores(
        fnc AddDelta(int delta, int input): int { return input + delta; }
        define model Bad(delta: int = -9223372036854775809) as
            AddDelta(delta)
        end
    )ores");
    expect_error("MODEL_CONST_OVERFLOW", R"ores(
        fnc AddDelta(int delta, int input): int { return input + delta; }
        define model Bad as
            AddDelta(9223372036854775808)
        end
    )ores");
    expect_error("MODEL_CONST_OVERFLOW", R"ores(
        fnc AddDelta(int delta, int input): int { return input + delta; }
        define model Bad as
            AddDelta(-9223372036854775809)
        end
    )ores");

    expect_error("MODEL_NONCONSTANT_CONFIG", R"ores(
        fnc Identity(int input): int { return input; }
        define model Configured(delta: int) as
            Identity()
        end
        fnc use(int dynamic): int {
            return Configured(dynamic).run(1);
        }
    )ores");

    expect_error("MODEL_EFFECTFUL_BODY", R"ores(
        fnc Helper(int input): int { return input; }
        fnc HiddenCall(int input): int { return Helper(input); }
        define model Bad as
            HiddenCall()
        end
    )ores");

    expect_error("MODEL_RECURSIVE_COMPOSITION", R"ores(
        define model A as
            B()
        end
        define model B as
            A()
        end
    )ores");

    expect_error("MODEL_VALUE_STORAGE_UNSUPPORTED", R"ores(
        fnc Identity(int input): int { return input; }
        define model Stored as
            Identity()
        end
        fnc use(): int {
            return Stored();
        }
    )ores");

    return 0;
}
