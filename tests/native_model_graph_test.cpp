#include "oreslang/frontend/ModelGraph.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

namespace {

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
           "a65fbdcab7c63eccdb5ec19409decb2da18ca9598d4c36a773f92e8b6521531d");
    assert(program.model_runs.front().blueprint_graph_digest ==
           program.models.front().graph_digest);
    assert(program.model_runs.front().configured_graph_digest ==
           "4531d20945630eed7bb451d63ae73ca5637559c438f6e89be18bbcd96abe9473");
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
