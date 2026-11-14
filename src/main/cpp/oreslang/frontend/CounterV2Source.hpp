#pragma once

// Compiler-owned lexical/type admission for one canonical declared Counter v2
// package. This is NOT executable LLVM actor MIR or native guest codegen.
// Its typed plan must never be accepted as a runtime state-loan capability.
#include "oreslang/frontend/SourceInput.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang::frontend::counter_v2 {

// Canonical typed actor-state MIR. Loan IDs here are compile-time
// instruction-local labels, NEVER scheduler-issued runtime authority.
enum class MirOp {
    RequireActorTurn, AcquireWrite, LoadStateI64, LoadMailI64,
    CheckedAddI64, StoreStateI64, ReleaseWrite,
    AcquireRead, CopyStateI64, ReleaseRead,
    MailboxSendCopyI64, CheckStop, ReturnVoid
};
enum class LoanKind { Read, Write };
enum class LoanAction { Borrow, Release };
struct SymbolicLoan {
    std::string field;
    std::uint32_t identity = 0;
    LoanKind kind = LoanKind::Read;
    LoanAction action = LoanAction::Borrow;
    bool generation_fence_required = false;
    bool current_actor_turn_required = false;
};
inline constexpr std::array<MirOp, 13> kReceiveMir{{
    MirOp::RequireActorTurn, MirOp::AcquireWrite,
    MirOp::LoadStateI64, MirOp::LoadMailI64,
    MirOp::CheckedAddI64, MirOp::StoreStateI64,
    MirOp::ReleaseWrite, MirOp::AcquireRead,
    MirOp::CopyStateI64, MirOp::ReleaseRead,
    MirOp::MailboxSendCopyI64, MirOp::CheckStop, MirOp::ReturnVoid
}};
static_assert(kReceiveMir[6] == MirOp::ReleaseWrite &&
              kReceiveMir[7] == MirOp::AcquireRead &&
              kReceiveMir[9] == MirOp::ReleaseRead &&
              kReceiveMir[10] == MirOp::MailboxSendCopyI64,
              "actor mailbox send must follow detached read loan release");

struct VerifiedPlan {
    std::string name_space;
    std::string actor;
    std::string state_field;
    std::int64_t initial = 0;
    std::int64_t stop = 0;
    std::array<std::int64_t, 4> messages{};
    std::array<MirOp, 13> receive_mir = kReceiveMir;
    std::array<SymbolicLoan, 4> symbolic_loans{};
    // This plan is semantic compiler metadata, not executable guest IR.
    // Only the actor runtime may supply actual generation-fenced permits.
};

// Keep source offsets to distinguish adjacent critical-read/write from
// ambiguous subtraction tokens. Comments never create executable tokens.
struct Token {
    std::string text;
    std::size_t offset = 0;
};

inline bool initial(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool ident_char(char c) { return initial(c) || digit(c); }

inline std::vector<Token> lex(std::string_view source) {
    std::vector<Token> result;
    for (std::size_t i = 0; i < source.size();) {
        const unsigned char c = static_cast<unsigned char>(source[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if (source.substr(i, 2) == "//") {
            i += 2;
            while (i < source.size() && source[i] != '\r' && source[i] != '\n') ++i;
            continue;
        }
        if (source.substr(i, 2) == "/*") {
            i += 2;
            unsigned depth = 1;
            while (depth != 0) {
                if (i >= source.size()) throw std::runtime_error("Counter v2 unterminated comment");
                if (source.substr(i, 2) == "/*") {
                    if (++depth > 64) throw std::runtime_error("Counter v2 comment depth");
                    i += 2;
                } else if (source.substr(i, 2) == "*/") {
                    --depth;
                    i += 2;
                } else ++i;
            }
            continue;
        }
        const auto start = i;
        if (initial(static_cast<char>(c))) {
            ++i;
            while (i < source.size() && ident_char(source[i])) ++i;
        } else if (digit(static_cast<char>(c))) {
            ++i;
            while (i < source.size() && (digit(source[i]) || source[i] == '_')) ++i;
        } else if (source.substr(i, 2) == "->" || source.substr(i, 2) == "==") {
            i += 2;
        } else if (source.substr(i, 3) == "\"|\"") {
            i += 3;
        } else if (std::string_view(";:.,=+{}()|<>-").find(static_cast<char>(c)) !=
                   std::string_view::npos) {
            ++i;
        } else {
            throw std::runtime_error("Counter v2 unsupported lexical source");
        }
        result.push_back({std::string(source.substr(start, i - start)), start});
        if (result.size() > 4096) throw std::runtime_error("Counter v2 token budget");
    }
    result.push_back({"", source.size()});
    return result;
}

class Reader {
public:
    explicit Reader(std::string text) : source_(std::move(text)), tokens_(lex(source_)) {}

    bool take(std::string_view word) {
        if (tokens_[at_].text != word) return false;
        ++at_;
        return true;
    }
    void e(std::string_view word) {
        if (!take(word))
            throw std::runtime_error("Counter v2 expected '" + std::string(word) +
                                     "' near byte " + std::to_string(tokens_[at_].offset));
    }
    std::string id() {
        const auto& t = tokens_[at_].text;
        if (t.empty() || t.size() > 128 || !initial(t[0])) {
            throw std::runtime_error("Counter v2 identifier missing");
        }
        for (const char c : t)
            if (!ident_char(c)) throw std::runtime_error("Counter v2 invalid identifier");
        ++at_;
        return t;
    }
    std::int64_t i64() {
        const bool negative = take("-");
        const auto t = tokens_[at_].text;
        if (t.empty() || !digit(t.front()) || !digit(t.back()) ||
            t.find("__") != std::string::npos)
            throw std::runtime_error("Counter v2 malformed i64");
        std::string value;
        if (negative) value = "-";
        for (char c : t) {
            if (c == '_') continue;
            if (!digit(c)) throw std::runtime_error("Counter v2 malformed i64");
            value.push_back(c);
        }
        std::int64_t number = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("Counter v2 signed i64 overflow");
        ++at_;
        return number;
    }
    std::string name_space() {
        e("namespace");
        const auto result = id();
        e(";");
        return result;
    }
    void name_space(std::string_view expected) {
        if (name_space() != expected)
            throw std::runtime_error("Counter v2 cross-file namespace mismatch");
    }
    std::string loan(std::string_view mode) {
        const auto offset = tokens_[at_].offset;
        e("critical");
        if (tokens_[at_].offset != offset + 8)
            throw std::runtime_error("Counter v2 nonadjacent critical mode");
        e("-");
        if (tokens_[at_].offset != offset + 9)
            throw std::runtime_error("Counter v2 nonadjacent critical mode");
        e(mode);
        e("|");
        auto result = id();
        if (result == "self" || result == "mail" || result == "handle_state")
            throw std::runtime_error("Counter v2 reserved state-loan alias");
        e("|");
        e("->");
        return result;
    }
    void done() const {
        if (!tokens_[at_].text.empty())
            throw std::runtime_error("Counter v2 unsupported executable suffix");
    }

private:
    std::string source_;
    std::vector<Token> tokens_;
    std::size_t at_ = 0;
};

// Fixed package root/path enumeration. File identity checks use SourceInput's
// opened-descriptor size/time recheck, and every package component must be a
// real directory/file instead of an attacker-supplied symlink.
inline std::string source_file(const std::filesystem::path& root,
                               std::string_view unit) {
    const auto path = root / "src" / "counter" / std::string(unit);
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        throw std::runtime_error("Counter v2 source must be real regular file");
    const auto native = path.string();
    auto text = read_regular_source(native.c_str(), 65536,
                                    "Counter v2 source file exceeds 64 KiB");
    require_valid_source_utf8(text);
    if (text.size() >= 3 && text.compare(0, 3, "\xef\xbb\xbf") == 0)
        throw std::runtime_error("Counter v2 source BOM forbidden");
    return text;
}

inline VerifiedPlan verify(const std::filesystem::path& root) {
    for (const auto& dir : {root, root / "src", root / "src" / "counter"}) {
        std::error_code status_error;
        const auto stat = std::filesystem::symlink_status(dir, status_error);
        if (status_error || !std::filesystem::is_directory(stat) ||
            std::filesystem::is_symlink(stat))
            throw std::runtime_error("Counter v2 source directory must be real");
    }
    const auto manifest = root / ".atto.pkg.toml";
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(manifest, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        throw std::runtime_error("Counter v2 package manifest must be real");
    const auto manifest_path = manifest.string();
    const auto config = read_regular_source(manifest_path.c_str(), 65536,
                                           "Counter v2 manifest too large");
    if (config != "schema_version = \"1\"\n[project]\nname = \"counter-v2\"\n"
                  "version = \"0.2.0\"\nroot = \".\"\n[source]\nroots = [\"src\"]\n")
        throw std::runtime_error("Counter v2 package manifest mismatch");

    VerifiedPlan plan;
    Reader actor(source_file(root, "actor.atto"));
    plan.name_space = actor.name_space();
    actor.e("declare"); actor.e("pub"); actor.e("actor");
    plan.actor = actor.id();
    actor.e("implements"); const auto contract_name = actor.id();
    actor.e(";"); actor.done();

    Reader contract(source_file(root, "contract.atto"));
    contract.name_space(plan.name_space);
    contract.e("define"); contract.e("pub"); contract.e("contract");
    contract.e(contract_name); contract.e("for"); contract.e("actor");
    contract.e("as"); contract.e("mtd");
    const auto method_name = contract.id();
    contract.e("("); contract.e("int"); const auto contract_arg = contract.id();
    contract.e(")"); contract.e(":"); contract.e("int"); contract.e(";");
    contract.e("end"); contract.done();

    Reader method(source_file(root, "method.atto"));
    method.name_space(plan.name_space);
    method.e("pub"); method.e("mtd"); method.e("("); method.e(plan.actor);
    const auto method_owner = method.id();
    method.e(")"); method.e(method_name); method.e("("); method.e("int");
    const auto method_arg = method.id();
    method.e(")"); method.e(":"); method.e("int"); method.e("{");
    method.e("return"); method.e(method_arg); method.e(";"); method.e("}");
    method.done();

    Reader state(source_file(root, "state.atto"));
    state.name_space(plan.name_space);
    state.e("define"); state.e("state"); state.e("on"); state.e("actor");
    state.e(plan.actor); state.e("as"); state.e("{");
    state.e("let"); state.e("int"); plan.state_field = state.id();
    state.e(":"); plan.initial = state.i64();
    state.e(";"); state.e("}"); state.done();

    Reader receiver(source_file(root, "receive.atto"));
    receiver.name_space(plan.name_space);
    receiver.e("external"); receiver.e("mtd"); receiver.e("(");
    receiver.e(plan.actor); const auto receiver_owner = receiver.id();
    receiver.e(")"); receiver.e("receive"); receiver.e("(");
    receiver.e("ActorMail"); receiver.e("<"); receiver.e("int"); receiver.e(">");
    const auto mail = receiver.id();
    receiver.e(")"); receiver.e(":"); receiver.e("void"); receiver.e("{");
    receiver.e("self"); receiver.e("."); receiver.e("handle_state"); receiver.e("(");
    const auto writer = receiver.loan("write");
    receiver.e("{");
    receiver.e(writer); receiver.e("."); receiver.e(plan.state_field);
    receiver.e("="); receiver.e(writer); receiver.e("."); receiver.e(plan.state_field);
    receiver.e("+"); receiver.e(mail); receiver.e("."); receiver.e("value");
    receiver.e(";"); receiver.e("}"); receiver.e(")"); receiver.e(";");

    // Compiler-authored read loan ends at the semicolon before send().
    receiver.e("val"); receiver.e("int"); const auto detached = receiver.id();
    receiver.e("="); receiver.e("self"); receiver.e("."); receiver.e("handle_state");
    receiver.e("("); const auto reader = receiver.loan("read");
    receiver.e(reader); receiver.e("."); receiver.e(plan.state_field);
    receiver.e(")"); receiver.e(";");
    receiver.e("self"); receiver.e("."); receiver.e("send"); receiver.e("(");
    receiver.e(detached); receiver.e(")"); receiver.e(";");

    receiver.e("if"); receiver.e(mail); receiver.e("."); receiver.e("value");
    receiver.e("=="); plan.stop = receiver.i64();
    receiver.e(";"); receiver.e("do"); receiver.e("self"); receiver.e(".");
    receiver.e("end"); receiver.e("("); receiver.e(")"); receiver.e(";");
    receiver.e("fi"); receiver.e("return"); receiver.e(";");
    receiver.e("}"); receiver.done();

    const auto reserved = [](const std::string& name) {
        return name == "self" || name == "handle_state" || name == "send" ||
               name == "receive" || name == "end" || name == "mail";
    };
    if (reserved(plan.actor) || reserved(plan.state_field) || reserved(detached) ||
        reserved(writer) || reserved(reader) || reserved(receiver_owner) ||
        reserved(method_owner) || detached == mail || detached == writer ||
        detached == reader || detached == plan.state_field ||
        detached == plan.actor || detached == receiver_owner ||
        mail == reader || mail == writer || mail == receiver_owner ||
        method_owner == method_arg || contract_arg == method_name)
        throw std::runtime_error("Counter v2 actor/loan/binding authority shadowed");

    Reader main(source_file(root, "main.atto"));
    main.name_space(plan.name_space);
    main.e("pub"); main.e("async"); main.e("routine"); main.e("main");
    main.e("("); main.e(")"); main.e(":"); main.e("void"); main.e("{");
    main.e("val"); const auto worker = main.id();
    if (reserved(worker)) throw std::runtime_error("Counter v2 reserved worker binding");
    main.e("="); main.e("spawn"); main.e(plan.actor);
    main.e("("); main.e(")"); main.e(";");
    main.e("await"); main.e(worker); main.e("."); main.e("get_ready");
    main.e("("); main.e(")"); main.e(";");

    std::int64_t total = plan.initial;
    for (std::size_t i = 0; i < plan.messages.size(); ++i) {
        main.e(worker); main.e("."); main.e("send"); main.e("(");
        const auto incoming = main.i64();
        main.e(")"); main.e(";");
        if (i != plan.messages.size() - 1 && incoming == plan.stop)
            throw std::runtime_error("Counter v2 early STOP before final send");
        if ((incoming > 0 && total > std::numeric_limits<std::int64_t>::max() - incoming) ||
            (incoming < 0 && total < std::numeric_limits<std::int64_t>::min() - incoming))
            throw std::runtime_error("Counter v2 native actor state would overflow");
        total += incoming;
        plan.messages[i] = incoming;
    }
    if (plan.messages.back() != plan.stop)
        throw std::runtime_error("Counter v2 missing terminal STOP");

    std::array<std::string, 4> outputs;
    std::set<std::string> bindings{worker};
    for (auto& output : outputs) {
        main.e("val"); output = main.id();
        if (reserved(output) || !bindings.insert(output).second)
            throw std::runtime_error("Counter v2 duplicate result name");
        main.e("="); main.e("("); main.e("await"); main.e(worker);
        main.e("."); main.e("outputs"); main.e("."); main.e("next");
        main.e("("); main.e(")"); main.e(")"); main.e(".");
        main.e("value"); main.e("."); main.e("unwrap");
        main.e("("); main.e(")"); main.e(";");
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        main.e("stdio"); main.e("."); main.e("stdout"); main.e(".");
        main.e("write"); main.e("("); main.e(outputs[i]);
        main.e("."); main.e("value"); main.e(")"); main.e(";");
        if (i + 1 < outputs.size()) {
            main.e("stdio"); main.e("."); main.e("stdout"); main.e(".");
            main.e("write"); main.e("("); main.e("\"|\"");
            main.e(")"); main.e(";");
        }
    }
    main.e("await"); main.e(worker); main.e("."); main.e("get_done");
    main.e("("); main.e(")"); main.e(";"); main.e("return");
    main.e(";"); main.e("}"); main.done();

    // The write permit is released before starting the read permit,
    // and the detached scalar is copied before the reader is released.
    // The mailbox instruction follows both releases. No runtime loan
    // identity is ever constructed by this compile-time verifier.
    plan.symbolic_loans = {{
        {plan.state_field, 1u, LoanKind::Write, LoanAction::Borrow, true, true},
        {plan.state_field, 1u, LoanKind::Write, LoanAction::Release, true, true},
        {plan.state_field, 2u, LoanKind::Read, LoanAction::Borrow, true, true},
        {plan.state_field, 2u, LoanKind::Read, LoanAction::Release, true, true}
    }};
    if (plan.receive_mir != kReceiveMir ||
        plan.symbolic_loans[0].identity == plan.symbolic_loans[2].identity ||
        plan.symbolic_loans[0].kind != LoanKind::Write ||
        plan.symbolic_loans[1].action != LoanAction::Release ||
        plan.symbolic_loans[2].kind != LoanKind::Read ||
        plan.symbolic_loans[3].action != LoanAction::Release ||
        plan.symbolic_loans[0].field != plan.symbolic_loans[2].field)
        throw std::runtime_error("Counter v2 invalid typed actor state MIR");
    // Source-verified symbolic MIR is not executable LLVM codegen.
    return plan;
}
} // namespace oreslang::frontend::counter_v2
