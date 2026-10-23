#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/ReachabilityManifest.hpp"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

using namespace oreslang;

namespace {

bool contains(
        const std::vector<SemanticDeclarationId>& values,
        const SemanticDeclarationId& value) {
    return std::binary_search(values.begin(), values.end(), value);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;

    {
        std::ifstream source_file(argv[1], std::ios::binary);
        std::ifstream expected_file(argv[2], std::ios::binary);
        assert(source_file.good());
        assert(expected_file.good());

        std::ostringstream source_buffer;
        std::ostringstream expected_buffer;
        source_buffer << source_file.rdbuf();
        expected_buffer << expected_file.rdbuf();

        const Program program = parse_program(source_buffer.str());
        const auto result = analyze_semantic_reachability(program);
        const auto manifest = make_reachability_manifest_v1(
            program, result);
        std::string expected = expected_buffer.str();
        if (!expected.empty() && expected.back() == '\n')
            expected.pop_back();
        assert(canonical_reachability_manifest_json(manifest) == expected);
    }


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
pub define contract Runnable as
  fnc run(int value) => int;
end
define module Service conforms Runnable as
  pub fnc run(int value): int { return value + 1; }
  pub fnc main(): int { return 0; }
end
)ores";
        const Program program = parse_program(source);
        assert(program.contracts.size() == 1);
        assert(program.conforms.size() == 1);

        const auto result = analyze_semantic_reachability(program);
        const auto run_id =
            function_declaration_id(CallableId{"run", 1});
        const auto main_id =
            function_declaration_id(CallableId{"main", 0});

        // Same-file structural contracts are full-source admission evidence.
        // In a closed executable they do not become runtime roots by themselves.
        assert(contains(result.roots, main_id));
        assert(contains(result.removed, run_id));
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
