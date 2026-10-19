#include "oreslang/types/EbpfXdpAdmission.hpp"
#include <iostream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

static void check(bool value) {
    if (!value) throw std::runtime_error("eBPF XDP admission regression failed");
}
static std::unique_ptr<oreslang::Expression> lit(std::int64_t v) {
    auto e = std::make_unique<oreslang::Expression>();
    e->kind = oreslang::Expression::Kind::Integer;
    e->integer = v;
    return e;
}
static oreslang::Program clean() {
    oreslang::Program program;
    oreslang::Function fn;
    fn.name = "xdp_action";
    fn.exported = true;
    oreslang::Parameter p;
    p.name = "packet_nonempty";
    fn.parameters.push_back(p);
    fn.result = lit(2);
    program.functions.push_back(std::move(fn));
    return program;
}
static void reject(oreslang::Program p) {
    bool denied = false;
    try {
        oreslang::EbpfXdpAdmission::require(p);
    } catch (const std::runtime_error&) {
        denied = true;
    }
    check(denied);
}
int main() {
    oreslang::EbpfXdpAdmission::require(clean());
    {
        auto p = clean();
        oreslang::GuardedReturn g;
        g.lhs = std::make_unique<oreslang::Expression>();
        g.lhs->kind = oreslang::Expression::Kind::Variable;
        g.lhs->name = "packet_nonempty";
        g.rhs = lit(0);
        g.result = lit(1);
        p.functions.front().early_returns.push_back(std::move(g));
        oreslang::EbpfXdpAdmission::require(p);
    }
    {
        // The entire possible runtime input domain is {0,1}; both outcomes
        // are admitted by static action proof for this arithmetic expression.
        auto p = clean();
        p.functions.front().result->kind = oreslang::Expression::Kind::Add;
        p.functions.front().result->left =
            std::make_unique<oreslang::Expression>();
        p.functions.front().result->left->kind =
            oreslang::Expression::Kind::Variable;
        p.functions.front().result->left->name = "packet_nonempty";
        p.functions.front().result->right = lit(1);
        oreslang::EbpfXdpAdmission::require(p);
    }
    { auto p = clean(); p.functions.front().result = lit(0); reject(std::move(p)); }
    { auto p = clean(); p.functions.front().result = lit(3); reject(std::move(p)); }
    { auto p = clean(); p.functions.front().result = lit(4); reject(std::move(p)); }
    { auto p = clean(); p.functions.front().result = lit(-1); reject(std::move(p)); }
    {
        auto p = clean();
        p.functions.front().result->kind = oreslang::Expression::Kind::Variable;
        p.functions.front().result->name = "packet_nonempty";
        reject(std::move(p)); // input=0 would yield XDP_ABORTED
    }
    {
        auto p = clean();
        oreslang::GuardedReturn g;
        g.lhs = std::make_unique<oreslang::Expression>();
        g.lhs->kind = oreslang::Expression::Kind::Variable;
        g.lhs->name = "packet_nonempty";
        g.rhs = lit(1);
        g.result = lit(4);
        p.functions.front().early_returns.push_back(std::move(g));
        reject(std::move(p)); // XDP_REDIRECT without helper/map
    }
    { auto p = clean(); p.classes.emplace_back(); reject(std::move(p)); }
    { auto p = clean(); p.module = "unsafe"; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().name = "main"; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().exported = false; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().parameters.clear(); reject(std::move(p)); }
    { auto p = clean(); p.functions.front().parameters.front().name = "ctx"; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().parameters.front().mode = oreslang::Parameter::Mode::Borrow; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().returns_void = true; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().locals.emplace_back(); reject(std::move(p)); }
    { auto p = clean(); p.functions.front().has_declared_effects = true; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().early_returns.resize(9); reject(std::move(p)); }
    { auto p = clean(); p.functions.front().actor_actions.push_back({oreslang::ActorAction::Kind::Send,0}); reject(std::move(p)); }
    { auto p = clean(); p.functions.front().result->kind = oreslang::Expression::Kind::Call; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().result->kind = oreslang::Expression::Kind::Borrow; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().result->kind = oreslang::Expression::Kind::Variable; p.functions.front().result->name = "ctx"; reject(std::move(p)); }
    { auto p = clean(); p.functions.front().result->type_arguments_present = true; reject(std::move(p)); }
    {
        auto p = clean();
        for (int i = 0; i < 17; ++i) {
            auto parent = std::make_unique<oreslang::Expression>();
            parent->kind = oreslang::Expression::Kind::Negate;
            parent->left = std::move(p.functions.front().result);
            p.functions.front().result = std::move(parent);
        }
        reject(std::move(p));
    }
    {
        auto p = clean();
        for (int i = 0; i < 129; ++i) {
            auto parent = std::make_unique<oreslang::Expression>();
            parent->kind = oreslang::Expression::Kind::Add;
            parent->left = std::move(p.functions.front().result);
            parent->right = lit(1);
            p.functions.front().result = std::move(parent);
        }
        reject(std::move(p));
    }
    {
        // A balanced expression exceeds the TOTAL node budget while staying
        // well within the separate depth limit. This checks both controls.
        auto p = clean();
        std::function<std::unique_ptr<oreslang::Expression>(int)> balanced =
            [&](int depth) -> std::unique_ptr<oreslang::Expression> {
                if (depth == 0) return lit(1);
                auto parent = std::make_unique<oreslang::Expression>();
                parent->kind = oreslang::Expression::Kind::Add;
                parent->left = balanced(depth - 1);
                parent->right = balanced(depth - 1);
                return parent;
            };
        p.functions.front().result = balanced(7); // 255 AST nodes, depth=7
        reject(std::move(p));
    }
    std::cout << "PASS: strict XDP policy AST admission / negative effect and budget matrix\n";
}
