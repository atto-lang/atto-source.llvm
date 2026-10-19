#pragma once
#include "oreslang/frontend/Parser.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
namespace oreslang {
// Strict, source-AST admission. This is an experimental BPF IR subset only,
// NOT a certificate of Linux kernel verifier acceptance.
struct EbpfXdpAdmission final {
  static void require(const Program& source) {
    if (!source.module.empty() || !source.classes.empty() || !source.actors.empty() ||
        !source.cooperative_loops.empty() || source.functions.size() != 1)
      throw std::runtime_error("eBPF XDP requires one pure scalar function and no modules/actors/classes");
    const auto& fn = source.functions.front();
    if (fn.name != "xdp_action" || !fn.exported || fn.returns_void || !fn.result ||
        fn.parameters.size() != 1 || fn.parameters.front().name != "packet_nonempty" ||
        fn.parameters.front().mode != Parameter::Mode::Value ||
        !fn.parameters.front().class_name.empty() ||
        !fn.parameters.front().generic_type.empty() ||
        !fn.generic_parameters.empty() || fn.scalar_while ||
        !fn.locals.empty() || !fn.statements.empty() || fn.early_returns.size() > 8 ||
        !fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
        !fn.actor_stops.empty() || !fn.actor_actions.empty() ||
        !fn.actor_field_writes.empty() || fn.caught_throw_value ||
        !fn.caught_throw_defers.empty() || fn.has_declared_effects)
      throw std::runtime_error("eBPF XDP requires pub fnc xdp_action(int packet_nonempty): int, scalar return and equality guards only");
    std::size_t nodes = 0;
    check_expr(*fn.result, 0, nodes);
    for (const auto& guard : fn.early_returns) {
      if (!guard.lhs || !guard.rhs || !guard.result)
        throw std::runtime_error("eBPF XDP malformed guard rejected");
      check_expr(*guard.lhs, 0, nodes);
      check_expr(*guard.rhs, 0, nodes);
      check_expr(*guard.result, 0, nodes);
    }
    // The sole guest parameter is a checked 0/1 predicate. Enumerate the
    // complete input domain and prove that ALL reachable outcomes are actions
    // actually supported by this prototype. Do NOT permit XDP_REDIRECT (4)
    // without setting redirect metadata through an approved helper/map, nor
    // XDP_TX (3) before a packet/device ownership policy is established.
    for (std::uint64_t input : {std::uint64_t{0}, std::uint64_t{1}}) {
      const auto action = evaluate_function(fn, input);
      if (action != 1 && action != 2)
        throw std::runtime_error(
            "eBPF XDP policy outcome must be XDP_DROP(1) or XDP_PASS(2)");
    }
  }
private:
  // All arithmetic is 64-bit wrapping bitvector arithmetic. This avoids
  // undefined signed C++ overflow in the admission proof and mirrors LLVM's
  // current unflagged i64 add/sub/mul/neg lowering.
  static std::uint64_t evaluate_expr(const Expression& e, std::uint64_t bit) {
    using Kind = Expression::Kind;
    switch (e.kind) {
      case Kind::Integer: return static_cast<std::uint64_t>(e.integer);
      case Kind::Variable: return bit;
      case Kind::Negate: return std::uint64_t{0} - evaluate_expr(*e.left, bit);
      case Kind::Add: return evaluate_expr(*e.left, bit) + evaluate_expr(*e.right, bit);
      case Kind::Subtract: return evaluate_expr(*e.left, bit) - evaluate_expr(*e.right, bit);
      case Kind::Multiply: return evaluate_expr(*e.left, bit) * evaluate_expr(*e.right, bit);
      default: throw std::runtime_error("eBPF XDP unadmitted effect in action proof");
    }
  }
  static std::uint64_t evaluate_function(const Function& fn, std::uint64_t bit) {
    for (const auto& guard : fn.early_returns) {
      if (evaluate_expr(*guard.lhs, bit) == evaluate_expr(*guard.rhs, bit))
        return evaluate_expr(*guard.result, bit);
    }
    return evaluate_expr(*fn.result, bit);
  }
  static void check_expr(const Expression& e, std::size_t depth, std::size_t& nodes) {
    if (depth > 16 || ++nodes > 128)
      throw std::runtime_error("eBPF XDP scalar expression budget exceeded");
    if (!e.arguments.empty() || !e.argument_names.empty() ||
        !e.type_arguments.empty() || e.type_arguments_present)
      throw std::runtime_error("eBPF XDP rejects calls/type-argument syntax");
    using Kind = Expression::Kind;
    switch (e.kind) {
      case Kind::Integer:
        if (e.left || e.right || !e.name.empty())
          throw std::runtime_error("eBPF XDP malformed integer node");
        return;
      case Kind::Variable:
        if (e.left || e.right || e.name != "packet_nonempty")
          throw std::runtime_error("eBPF XDP only permits packet_nonempty variable");
        return;
      case Kind::Negate:
        if (!e.left || e.right) break;
        check_expr(*e.left, depth + 1, nodes); return;
      case Kind::Add: case Kind::Subtract: case Kind::Multiply:
        if (!e.left || !e.right) break;
        check_expr(*e.left, depth + 1, nodes);
        check_expr(*e.right, depth + 1, nodes); return;
      default:
        throw std::runtime_error("eBPF XDP rejects heap, field, call, move, borrow and unsupported expression effects");
    }
    throw std::runtime_error("eBPF XDP malformed arithmetic expression");
  }
};
} // namespace oreslang
