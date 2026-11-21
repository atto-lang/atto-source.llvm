#pragma once

#include <cstdint>

namespace oreslang::runtime {

// Compiler-owned, checked i64 instruction ABI. Every instruction is an
// (opcode, signed-operand) pair in immutable compiler-emitted module memory.
// No guest source can invoke this ABI or provide a native pointer directly.
enum class ActorScalarOp : std::uint64_t {
    Constant = 1,
    Argument = 2,
    Add = 3,
    Subtract = 4,
    Multiply = 5,
    Negate = 6,
    Equal = 7,
    NotEqual = 8,
    Less = 9,
    LessEqual = 10,
    Greater = 11,
    GreaterEqual = 12,
    LogicalNot = 13,
    LogicalXor = 14,
    Call = 15,
    Return = 16,
    JumpIfZero = 17,
    Jump = 18,
};

inline constexpr std::uint64_t actor_stackless_m3_max_words = 4096;
inline constexpr std::uint64_t actor_stackless_m3_max_steps = 200000;

// These compiler-only symbols require exclusive MultiActorCarrier TLS
// authorization. The return is a scalar value, never a guest memory address.
// Invalid frames, overflow, fuel exhaustion, or lifecycle breach fail closed.
extern "C" std::int64_t oreslang_actor_guest_stackless_i64_execute(
    const std::uint64_t* program, std::uint64_t word_count,
    std::uint64_t entry_instruction, std::int64_t argument) noexcept;

} // namespace oreslang::runtime
