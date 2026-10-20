#include "oreslang/frontend/Parser.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#define CHECK(expr) do { if (!(expr)) std::abort(); } while (false)

namespace {

void expect_error(std::string_view source, std::string_view needle) {
    bool rejected = false;
    try {
        (void)oreslang::parse_program(source);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find(needle) != std::string_view::npos;
    }
    CHECK(rejected);
}

} // namespace

int main() {
    // A syntactically valid model reaches the explicit semantic gate, proving
    // the frontend consumed the declaration rather than routing it through an
    // ordinary function/class path.
    expect_error(R"(
        define model Price(features: int = 784) as
            Input(features)
            Scale(2, factor: 3)
        end
    )", "MODEL_LOWERING_NOT_IMPLEMENTED");

    expect_error(R"(
        define pub model PublicPrice as
            Source()
        end
    )", "MODEL_LOWERING_NOT_IMPLEMENTED");

    expect_error(R"(
        define module finance as
            define model pub Price as
                Source()
                Round()
            end
        end
    )", "MODEL_LOWERING_NOT_IMPLEMENTED");

    expect_error(R"(
        define model Empty as
        end
    )", "MODEL_EMPTY_BODY");

    expect_error(R"(
        define model Bad as
            Source()
            1 + 2
        end
    )", "MODEL_NON_LAYER_EXPRESSION");

    expect_error(R"(
        define model Duplicate(x: int, x: int) as
            Source()
        end
    )", "duplicate model parameter 'x'");

    expect_error(R"(
        define model DuplicateNamed as
            Op(x: 1, x: 2)
        end
    )", "duplicate named model argument 'x'");

    expect_error(R"(
        define model PositionalAfterNamed as
            Op(x: 1, 2)
        end
    )", "positional model arguments must precede named arguments");

    // model is contextual, not globally reserved.
    const auto ordinary = oreslang::parse_program(R"(
        fnc model(int x): int {
            return x;
        }
        pub fnc main(): int {
            return model(3);
        }
    )");
    CHECK(ordinary.functions.size() == 2);
    CHECK(ordinary.models.empty());
}
