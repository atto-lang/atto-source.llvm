#include "parametric_bundle_refinement.hpp"
#include <iostream>
#include <variant>

using namespace atto::parametric;
using V = std::variant<std::string, int>;
using A = Argument<V>;
static void require(bool ok) { if (!ok) throw std::runtime_error("check failed"); }
template<class F> static void rejects(F f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("accepted invalid input");
}
int main() {
    const std::vector<Formal<V>> schema{
        {"type", [](const V& v) { return std::holds_alternative<std::string>(v); }},
        {"size", [](const V& v) { auto n = std::get_if<int>(&v); return n && *n >= 0; }}
    };
    const std::vector<A> seed{{"size", 5}, {"type", std::string("int")}};
    const std::vector<A> expected{{"type", std::string("int")}, {"size", 6}};
    const auto run = [&](const std::vector<A>& first, const std::vector<A>& next, Mode mode = Mode::refinement) {
        return bind(schema, first, next, mode);
    };
    auto result = run(seed, {{"size", 6}});
    require(result == expected);
    require(seed[0].second == V(5));
    require(result == run({seed[1], seed[0]}, {{"size", 6}}));
    require(result == run({seed[1]}, {{"size", 6}}));
    require(run(seed, {})[1].second == V(5));
    require(result == run({seed[1]}, {{"size", 6}}, Mode::application));
    require(run(seed, {}, Mode::application)[1].second == V(5));
    rejects([&] { run(seed, {{"size", 6}, {"size", 6}}); });
    rejects([&] { run({seed[0], seed[0]}, {}); });
    rejects([&] { run(seed, {{"unknown", 1}}); });
    rejects([&] { run(seed, {{"size", std::string("6")}}); });
    rejects([&] { run(seed, {{"size", -1}}); });
    rejects([&] { bind(std::vector<Formal<V>>{schema[0], schema[0]}, seed, {}, Mode::refinement); });
    rejects([&] { run(seed, {{"size", 5}}, Mode::application); });
    rejects([&] { run(seed, {{"size", 6}}, Mode::application); });
    rejects([&] { run(seed, {{"unknown", 1}}, Mode::application); });
    result[1].second = 9;
    require(seed[0].second == V(5));
    require(run(seed, {{"size", 6}}) == expected);
    const auto infer = [&](const std::vector<A>& explicit_values, const std::vector<std::string>& holes,
                           const std::vector<A>& evidence) {
        return refine_inferred(schema, seed, explicit_values, holes, evidence);
    };
    require(infer({}, {"size"}, {{"size", 3}}) == std::vector<A>{{"type", std::string("int")}, {"size", 3}});
    require(seed[0].second == V(5));
    require(infer({}, {"size"}, {{"size", 0}})[1].second == V(0));
    rejects([&] { infer({}, {"size"}, {}); });
    rejects([&] { infer({}, {"size"}, {{"size", 3}, {"size", 3}}); });
    rejects([&] { infer({{"size", 6}}, {"size"}, {{"size", 3}}); });
    rejects([&] { infer({}, {"size"}, {{"size", 3}, {"type", std::string("int")}}); });
    rejects([&] { infer({}, {"size", "size"}, {{"size", 3}}); });
    rejects([&] { refine_inferred(schema, std::vector<A>{seed[1]}, {}, {"size"}, {{"size", 3}}); });
    rejects([&] { infer({}, {"size"}, {{"size", -1}}); });
    rejects([&] { infer({}, {"size"}, {{"size", std::string("3")}}); });
    std::cout << "29 host checks passed\n";
}
