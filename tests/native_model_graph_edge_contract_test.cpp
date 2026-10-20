#include "oreslang/frontend/PortableModelGraphDigestV1.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace p = oreslang::model::portable_v1;

static void require(bool ok, const char* explanation) {
    if (!ok) throw std::runtime_error(explanation);
}
static p::Signature sig(std::vector<p::Port> inputs) {
    return {std::move(inputs), p::NominalPort{"finance.Price@v1", 16}, "pure",
        false, 0, "ores.model.op.v1", std::string(64, 'a'), "verified-ores", {},
        p::Profile{true, "none", true}};
}
static p::Graph graph(std::vector<std::string> edges, std::vector<p::Port> inputs) {
    return {1, "finance.PriceEstimate@v1", {
        {"source", "PriceInput", {}, {}, sig({})},
        {"apply", "ApplyTax", std::move(edges), {}, sig(std::move(inputs))}
    }, {"apply"}};
}
static void deny(const char* code, const p::Graph& example) {
    try { (void)p::digest(example); }
    catch (const std::exception& e) {
        require(std::string_view(e.what()) == code, "wrong model edge diagnostic");
        return;
    }
    throw std::runtime_error("invalid model graph acquired a portable digest");
}
int main() {
    try {
        p::NominalPort price{"finance.Price@v1", 16};
        p::NominalPort mass{"finance.Weight@v1", 16};

        p::Graph external_unary{1, "finance.PriceTransform@v1", {
            {"input", "ValidatePrice", {}, {}, sig({price})}
        }, {"input"}};
        require(p::digest(external_unary).size() == 64,
                "unary external input boundary must be digestible");
        p::Graph external_binary{1, "finance.Bad@v1", {
            {"input", "BinaryInput", {}, {}, sig({price, price})}
        }, {"input"}};
        deny("MODEL_PORT_ARITY_MISMATCH", external_binary);
        deny("MODEL_PORT_ARITY_MISMATCH", graph({}, {price}));
        deny("MODEL_PORT_ARITY_MISMATCH", graph({"source"}, {}));
        deny("MODEL_PORT_ARITY_MISMATCH", graph({"source", "source"}, {price}));
        deny("MODEL_PORT_TYPE_MISMATCH", graph({"source"}, {mass}));
        deny("MODEL_PORT_TYPE_MISMATCH", graph({"source"}, {
            p::BufferPort{"f64", {2}, "contiguous", "USD"}}));
        deny("MODEL_FORWARD_OR_CYCLIC_EDGE", graph({"missing"}, {price}));

        auto too_big = graph({"source"}, {price});
        too_big.nodes[1].signature.output =
            p::BufferPort{"f64", {static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())}, "contiguous", std::nullopt};
        deny("MODEL_SIZE_OVERFLOW", too_big);
        auto zero_extent = graph({"source"}, {price});
        zero_extent.nodes[1].signature.output =
            p::BufferPort{"f64", {static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), 0}, "contiguous", std::nullopt};
        require(p::digest(zero_extent).size() == 64, "zero extents must not overflow");
        require(p::digest(graph({"source"}, {price})).size() == 64,
                "valid model cannot be digested");
        std::cout << "PASS: native resolved graph edge/port admission\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
