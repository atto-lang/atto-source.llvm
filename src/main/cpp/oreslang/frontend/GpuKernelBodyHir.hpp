#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::gpu::body_hir {

// First compiler-checked device-body slice. This is *not* device code, a
// verified buffer handle, an effect authority, or a permit to dispatch.
enum class Type { F32, I64, U64, ReadF32, WriteF32 };
enum class TokenKind { Identifier, Integer, Symbol };
struct Token { TokenKind kind; std::string text; };
struct Parameter { std::string name; Type type; };
enum class Opcode { GlobalId, BoundsGuard, LoadF32, MultiplyF32, AddF32, StoreF32, Return };
enum class Guard { LessInside, GreaterEqualEarlyReturn, NoOp };
struct Instruction {
    Opcode opcode;
    std::string operand;
};
struct CheckedBody {
    Guard guard;
    std::vector<Instruction> instructions;
    std::string lane;
    std::string extent;
    // The parser checked a *restricted source body*. It did not authenticate
    // host symbols, device allocations, queue generations, or execution.
    [[nodiscard]] constexpr bool source_body_typed() const noexcept { return true; }
    [[nodiscard]] constexpr bool executable() const noexcept { return false; }
    [[nodiscard]] constexpr bool device_code_available() const noexcept { return false; }
    [[nodiscard]] constexpr bool provider_loans_verified() const noexcept { return false; }
};

[[noreturn]] inline void reject(const char* code, const char* detail) {
    throw std::invalid_argument(std::string(code) + ": " + detail);
}

class Parser {
    const std::vector<Token>& tokens_;
    std::size_t at_ = 0;
    [[nodiscard]] bool at(std::string_view text) const {
        return at_ < tokens_.size() && tokens_[at_].text == text;
    }
    bool take(std::string_view text) {
        if (!at(text)) return false;
        ++at_;
        return true;
    }
    void need(std::string_view text, const char* code, const char* reason) {
        if (!take(text)) reject(code, reason);
    }
    [[nodiscard]] bool done() const { return at_ == tokens_.size(); }

    void store(const std::vector<Parameter>& p, const std::string& lane) {
        // Permitted shape: out[i] = alpha * x[i] + y[i];
        // The read/write view kinds, scalar types, and lane identity are
        // verified from the declaration; no implicit conversion is allowed.
        if (!at(p[3].name))
            reject("GPU-LAUNCH-005", "store must target exclusive output view");
        need(p[3].name, "GPU-LAUNCH-005", "invalid write target");
        need("[", "GPU-LAUNCH-005", "output index required");
        need(lane, "GPU-LAUNCH-005", "output index must be guarded lane");
        need("]", "GPU-LAUNCH-005", "output index must close");
        need("=", "GPU-LAUNCH-005", "output write required");
        need(p[0].name, "GPU-LAUNCH-002", "f32 multiplier required");
        need("*", "GPU-LAUNCH-002", "f32 multiplication required");
        need(p[1].name, "GPU-LAUNCH-005", "first immutable input view required");
        need("[", "GPU-LAUNCH-005", "first input index required");
        need(lane, "GPU-LAUNCH-005", "first input must use guarded lane");
        need("]", "GPU-LAUNCH-005", "first input index must close");
        need("+", "GPU-LAUNCH-002", "f32 addition required");
        need(p[2].name, "GPU-LAUNCH-005", "second immutable input view required");
        need("[", "GPU-LAUNCH-005", "second input index required");
        need(lane, "GPU-LAUNCH-005", "second input must use guarded lane");
        need("]", "GPU-LAUNCH-005", "second input index must close");
        need(";", "GPU-LAUNCH-008", "device store terminator required");
    }
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    CheckedBody check(const std::vector<Parameter>& params) {
        if (params.size() > 64 || tokens_.size() > 16384)
            reject("GPU-LAUNCH-008", "kernel source-body resource budget exceeded");
        std::unordered_set<std::string> names;
        for (const auto& p : params) {
            if (p.name.empty() || p.name.size() > 256 ||
                    !names.insert(p.name).second)
                reject("GPU-LAUNCH-001", "invalid or duplicate parameter identity");
        }
        for (const auto& token : tokens_) {
            if (token.text.empty() || token.text.size() > 256)
                reject("GPU-LAUNCH-008", "invalid source-body token");
        }
        if (tokens_.size() == 2 && tokens_[0].text == "return" &&
                tokens_[1].text == ";") {
            return {Guard::NoOp, {{Opcode::Return, ""}}, "", ""};
        }

        // Narrow v0: the five-argument SAXPY-like f32 kernel subset.
        if (params.size() != 5 || params[0].type != Type::F32 ||
                params[1].type != Type::ReadF32 ||
                params[2].type != Type::ReadF32 ||
                params[3].type != Type::WriteF32 ||
                (params[4].type != Type::I64 && params[4].type != Type::U64))
            reject("GPU-LAUNCH-008", "typed GPU body subset requires f32 SAXPY view signature");

        need("const", "GPU-LAUNCH-008", "lane declaration must be const");
        const bool signed_lane = take("int") || take("i64");
        const bool unsigned_lane = !signed_lane && take("u64");
        if (!signed_lane && !unsigned_lane)
            reject("GPU-LAUNCH-002", "lane requires explicit int/i64 or u64");
        if (signed_lane != (params[4].type == Type::I64))
            reject("GPU-LAUNCH-002", "lane and bound signedness must match exactly");
        if (at_ >= tokens_.size() || tokens_[at_].kind != TokenKind::Identifier)
            reject("GPU-LAUNCH-008", "lane needs local binding");
        const std::string lane = tokens_[at_++].text;
        if (lane.empty() || names.count(lane) != 0)
            reject("GPU-LAUNCH-001", "GPU lane shadows a kernel parameter");
        need("=", "GPU-LAUNCH-008", "lane initialization required");
        need("gpu", "GPU-LAUNCH-008", "lane must originate from gpu.global_id");
        need(".", "GPU-LAUNCH-008", "gpu member access required");
        need("global_id", "GPU-LAUNCH-008", "global_id required");
        need("(", "GPU-LAUNCH-008", "global_id axis required");
        need("0", "GPU-LAUNCH-003", "only axis 0 is currently supported");
        need(")", "GPU-LAUNCH-008", "global_id closing parenthesis required");
        need(";", "GPU-LAUNCH-008", "lane statement terminator required");

        if (!take("if"))
            reject("GPU-LAUNCH-005", "GPU memory access needs a dominating bounds guard");
        need(lane, "GPU-LAUNCH-005", "bounds guard must use the device lane");
        const bool inside = take("<");
        const bool early = !inside && take(">=");
        if (!inside && !early)
            reject("GPU-LAUNCH-005", "bounds guard must prove lane is below n");
        need(params[4].name, "GPU-LAUNCH-005", "guard must use declared bound");
        need("{", "GPU-LAUNCH-005", "guard requires braced body");

        CheckedBody result;
        result.guard = inside ? Guard::LessInside : Guard::GreaterEqualEarlyReturn;
        result.lane = lane;
        result.extent = params[4].name;
        result.instructions.push_back({Opcode::GlobalId, lane});
        result.instructions.push_back({Opcode::BoundsGuard, params[4].name});
        if (early) {
            need("return", "GPU-LAUNCH-005", "out-of-range lanes must return");
            need(";", "GPU-LAUNCH-005", "out-of-range early return must terminate");
            need("}", "GPU-LAUNCH-005", "bounds guard must close");
            take("fi");
            store(params, lane);
        } else {
            store(params, lane);
            need("}", "GPU-LAUNCH-005", "bounds guard must close");
            take("fi");
        }
        result.instructions.push_back({Opcode::LoadF32, params[1].name});
        result.instructions.push_back({Opcode::LoadF32, params[2].name});
        result.instructions.push_back({Opcode::MultiplyF32, params[0].name});
        result.instructions.push_back({Opcode::AddF32, params[2].name});
        result.instructions.push_back({Opcode::StoreF32, params[3].name});
        if (take("return")) {
            need(";", "GPU-LAUNCH-008", "return statement must terminate");
            result.instructions.push_back({Opcode::Return, ""});
        }
        if (!done())
            reject("GPU-LAUNCH-008", "unsupported trailing GPU operation or expression");
        return result;
    }
};

inline CheckedBody check(const std::vector<Parameter>& params,
                         const std::vector<Token>& tokens) {
    return Parser(tokens).check(params);
}

} // namespace oreslang::gpu::body_hir
