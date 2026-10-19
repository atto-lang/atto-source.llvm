#include "oreslang/types/EbpfXdpAdmission.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>

using namespace oreslang;

static std::unique_ptr<Expression> literal(std::int64_t value) {
    auto p = std::make_unique<Expression>();
    p->kind = Expression::Kind::Integer;
    p->integer = value;
    return p;
}
static std::unique_ptr<Expression> variable() {
    auto p = std::make_unique<Expression>();
    p->kind = Expression::Kind::Variable;
    p->name = "packet_nonempty";
    return p;
}
static Program make_program(std::unique_ptr<Expression> result) {
    Program p;
    Function fn;
    fn.name = "xdp_action";
    fn.exported = true;
    Parameter param;
    param.name = "packet_nonempty";
    fn.parameters.push_back(param);
    fn.result = std::move(result);
    p.functions.push_back(std::move(fn));
    return p;
}
static bool admitted(const Program& p) {
    try { EbpfXdpAdmission::require(p); return true; }
    catch (const std::runtime_error&) { return false; }
}

// Independent reference oracle: model the emitted unflagged i64 arithmetic
// with unsigned bitvectors. This is a full-domain proof only because the sole
// input is a checked bit, not an arbitrary packet length.
static std::uint64_t oracle(const Expression& e, std::uint64_t input) {
    using Kind = Expression::Kind;
    switch (e.kind) {
        case Kind::Integer: return static_cast<std::uint64_t>(e.integer);
        case Kind::Variable: return input;
        case Kind::Negate: return std::uint64_t{0} - oracle(*e.left, input);
        case Kind::Add: return oracle(*e.left, input) + oracle(*e.right, input);
        case Kind::Subtract: return oracle(*e.left, input) - oracle(*e.right, input);
        case Kind::Multiply: return oracle(*e.left, input) * oracle(*e.right, input);
        default: throw std::runtime_error("unsupported oracle expression");
    }
}
static std::unique_ptr<Expression> random_expression(std::mt19937_64& rng, unsigned depth) {
    const std::int64_t edges[] = {
        -1, 0, 1, 2, 3, 4,
        std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::min()
    };
    if (depth == 0 || (rng() % 5) == 0)
        return (rng() % 3) == 0 ? variable() : literal(edges[rng() % 8]);

    auto p = std::make_unique<Expression>();
    const auto op = rng() % 4;
    if (op == 0) {
        p->kind = Expression::Kind::Negate;
        p->left = random_expression(rng, depth - 1);
    } else {
        p->kind = op == 1 ? Expression::Kind::Add :
                  op == 2 ? Expression::Kind::Subtract :
                            Expression::Kind::Multiply;
        p->left = random_expression(rng, depth - 1);
        p->right = random_expression(rng, depth - 1);
    }
    return p;
}

int main() {
    if (!admitted(make_program(literal(1))) ||
        !admitted(make_program(literal(2))) ||
        admitted(make_program(literal(0))) ||
        admitted(make_program(literal(3))) ||
        admitted(make_program(literal(4))) ||
        admitted(make_program(literal(-1))))
        throw std::runtime_error("XDP action baseline failed");

    std::mt19937_64 rng(0x6B50465F4F524553ULL);
    std::size_t accepted = 0, rejected = 0;
    for (unsigned i = 0; i < 30000; ++i) {
        auto program = make_program(random_expression(rng, 5));
        const auto& expr = *program.functions.front().result;
        const auto first = oracle(expr, 0);
        const auto second = oracle(expr, 1);
        const bool expected = (first == 1 || first == 2) &&
                              (second == 1 || second == 2);
        const bool actual = admitted(program);
        if (actual != expected)
            throw std::runtime_error("XDP exhaustive action proof disagrees with reference oracle");
        if (actual) ++accepted; else ++rejected;
    }
    std::cout << "PASS: 30000 deterministic randomized XDP i64 action proofs; "
              << "accepted=" << accepted << " rejected=" << rejected << '\n';
}
