#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SemanticReachability.hpp"

#include <algorithm>
#include <cassert>
#include <string>

using namespace oreslang;

namespace {

bool contains(
        const std::vector<SemanticDeclarationId>& values,
        const SemanticDeclarationId& value) {
    return std::binary_search(values.begin(), values.end(), value);
}

} // namespace

int main() {
    {
        const std::string source = R"ores(
fnc branch_only(): int { return 42; }
fnc dead(): int { return 9; }
pub fnc main(): int {
  return true ? branch_only() : 0;
}
)ores";
        const Program program = parse_program(source);
        const auto result = analyze_semantic_reachability(program);
        assert(contains(
            result.retained,
            function_declaration_id(CallableId{"branch_only", 0})));
        assert(contains(
            result.removed,
            function_declaration_id(CallableId{"dead", 0})));
    }

    {
        const std::string source = R"ores(
fnc pick(int value): int { return value; }
fnc pick(int left, int right): int { return left * 10 + right; }
pub fnc main(): int {
  return pick(right: 2, left: 4) + pick(value: 1);
}
)ores";
        const Program program = parse_program(source);
        const auto result = analyze_semantic_reachability(program);
        assert(contains(
            result.retained,
            function_declaration_id(CallableId{"pick", 1})));
        assert(contains(
            result.retained,
            function_declaration_id(CallableId{"pick", 2})));
    }

    {
        const std::string source = R"ores(
define class Pair as
  pub final int a: start + 1;
  pub final mut int b: start * 2;
  constructor(int start) { return; }
end
pub fnc main(): int {
  let Pair p = new Pair(13);
  return p.a + p.b;
}
)ores";
        const Program program = parse_program(source);
        const auto result = analyze_semantic_reachability(program);
        assert(contains(
            result.retained,
            class_declaration_id("Pair")));
    }

    {
        const std::string source = R"ores(
define actor Worker as {
  let total: int = 0;
  pub fnc on_message(int n): int {
    total = total + n;
    return total;
  }
}
pub fnc main(): int {
  const worker = spawn Worker();
  worker.send(1);
  worker.send(2);
  stop worker;
  return 43;
}
)ores";
        const Program program = parse_program(source);
        const auto result = analyze_semantic_reachability(program);
        assert(contains(
            result.retained,
            actor_declaration_id("Worker")));
    }

    return 0;
}
