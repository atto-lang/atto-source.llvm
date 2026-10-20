#include "oreslang/frontend/ModelGraph.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <cassert>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* kSharedExecutableCorpusRevision =
    "model-core-i64-v1.2026-10-20.5";

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
           "model-core-i64-v1.2026-10-20.5");
    assert(oreslang::model_sha256("abc") ==
           "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");

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
    assert(program.model_runs.size() == 1);
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
