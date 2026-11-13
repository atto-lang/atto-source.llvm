#pragma once
// Declaration-header shape checks on already authenticated, in-memory Atto
// source bytes. NOT the production typed parser or a guest-codegen authority.
#include "oreslang/frontend/ActorSourceSetAdmission.hpp"
#include <cctype>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oreslang::frontend {

class ActorHeaderTokens {
public:
    explicit ActorHeaderTokens(std::string_view source) {
        std::size_t i = 0;
        while (i < source.size()) {
            const auto c = static_cast<unsigned char>(source[i]);
            if (c <= 32) { ++i; continue; }
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
                i += 2;
                while (i < source.size() && source[i] != '\n') ++i;
                continue;
            }
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
                i += 2;
                const auto end = source.find("*/", i);
                if (end == std::string_view::npos) reject("unterminated comment");
                i = end + 2;
                continue;
            }
            if (c == '"') {
                const auto start = i++;
                bool closed = false;
                while (i < source.size()) {
                    if (source[i] == '\\' && i + 1 < source.size()) { i += 2; continue; }
                    if (source[i++] == '"') { closed = true; break; }
                }
                if (!closed) reject("unterminated string literal");
                tokens_.emplace_back(source.substr(start, i - start));
                continue;
            }
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                c == '_' || (c >= '0' && c <= '9')) {
                const auto start = i++;
                while (i < source.size()) {
                    const auto next = static_cast<unsigned char>(source[i]);
                    if (!((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                          (next >= '0' && next <= '9') || next == '_'))
                        break;
                    ++i;
                }
                tokens_.emplace_back(source.substr(start, i - start));
                continue;
            }
            if (c == 127) reject("control byte in source");
            tokens_.emplace_back(source.substr(i, 1));
            ++i;
            if (tokens_.size() > 32768u)
                reject("header lexer token budget exceeded");
        }
    }

    void expect(std::string_view expected) {
        if (at_end() || tokens_[index_] != expected)
            reject("unexpected declaration header token");
        ++index_;
    }
    void expect_all(std::initializer_list<std::string_view> parts) {
        for (auto part : parts) expect(part);
    }
    std::string identifier() {
        if (at_end() || !source_set_identifier(tokens_[index_]))
            reject("invalid declaration identifier");
        return tokens_[index_++];
    }
    std::string decimal() {
        if (at_end() || tokens_[index_].empty() ||
            tokens_[index_].find_first_not_of("0123456789") != std::string::npos ||
            tokens_[index_].size() > 18)
            reject("invalid bounded decimal initializer");
        return tokens_[index_++];
    }
    bool consume(std::string_view value) {
        if (!at_end() && tokens_[index_] == value) { ++index_; return true; }
        return false;
    }
    void body_only() {
        expect("{");
        std::size_t depth = 1;
        while (depth > 0) {
            if (at_end()) reject("unterminated declaration body");
            const auto token = tokens_[index_++];
            if (token == "{") {
                if (++depth > 64) reject("declaration body nesting budget");
            } else if (token == "}") {
                --depth;
            }
        }
        finish();
    }
    void finish() const {
        if (!at_end()) reject("extra declaration after expected header");
    }
    bool at_end() const noexcept { return index_ == tokens_.size(); }
private:
    [[noreturn]] static void reject(const char* reason) {
        throw std::runtime_error(std::string("ACTOR_DECL_HEADER_REJECT: ") + reason);
    }
    std::vector<std::string> tokens_;
    std::size_t index_ = 0;
};

struct ActorDeclaredHeaderShape {
    std::string name_space;
    std::string actor;
    std::string contract;
    std::string state_field;
    std::string contract_method;
    bool external_receive_runtime_only = false;
};

inline void require_actor_header_namespace(ActorHeaderTokens& tokens,
                                           std::string_view expected) {
    tokens.consume("use");
    tokens.expect("namespace");
    tokens.expect(expected);
    tokens.expect(";");
}

// This recognizes *declaration headers only* on a proven byte snapshot;
// method bodies and critical/await effects are intentionally unverified.
inline ActorDeclaredHeaderShape verify_actor_declared_headers_only(
        const ActorVerifiedSourceSet& sources) {
    const auto& receipt = sources.receipt();
    ActorDeclaredHeaderShape shape{receipt.name_space, receipt.actor, receipt.contract, "", "", false};
    {
        ActorHeaderTokens t(sources.unit("actor").source);
        require_actor_header_namespace(t, shape.name_space);
        t.expect_all({"declare", "pub", "actor"});
        t.expect(shape.actor);
        t.expect("implements");
        t.expect(shape.contract);
        t.expect(";");
        t.finish();
    }
    {
        ActorHeaderTokens t(sources.unit("contract").source);
        require_actor_header_namespace(t, shape.name_space);
        t.expect_all({"define", "pub", "contract"});
        t.expect(shape.contract);
        t.expect_all({"for", "actor", "as", "mtd"});
        shape.contract_method = t.identifier();
        t.expect_all({"(", "int"});
        (void)t.identifier(); // parameter name, not a method capability
        t.expect_all({")", ":", "int", ";", "end"});
        t.finish();
    }
    {
        ActorHeaderTokens t(sources.unit("state").source);
        require_actor_header_namespace(t, shape.name_space);
        t.expect_all({"define", "state", "on", "actor"});
        t.expect(shape.actor);
        t.expect_all({"as", "{", "let", "int"});
        shape.state_field = t.identifier();
        t.expect(":");
        if (t.decimal() != "0")
            throw std::runtime_error("ACTOR_DECL_HEADER_REJECT: initial Counter state must be zero");
        t.expect_all({";", "}"});
        t.finish();
    }
    {
        ActorHeaderTokens t(sources.unit("method").source);
        require_actor_header_namespace(t, shape.name_space);
        t.expect_all({"pub", "mtd", "("});
        t.expect(shape.actor);
        (void)t.identifier(); // receiver binder is checked by future MIR
        t.expect(")");
        t.expect(shape.contract_method);
        t.expect_all({"(", "int"});
        (void)t.identifier();
        t.expect_all({")", ":", "int"});
        t.body_only();
    }
    {
        ActorHeaderTokens t(sources.unit("receive").source);
        require_actor_header_namespace(t, shape.name_space);
        // Explicitly PRIVATE runtime-only receive; an ordinary public
        // method or mismatched ActorMail parameter cannot claim this slot.
        t.expect_all({"external", "mtd", "("});
        t.expect(shape.actor);
        (void)t.identifier();
        t.expect_all({")", "receive", "(", "ActorMail", "<", "int", ">"});
        (void)t.identifier();
        t.expect_all({")", ":", "void"});
        t.body_only();
        shape.external_receive_runtime_only = true;
    }
    {
        ActorHeaderTokens t(sources.unit("main").source);
        require_actor_header_namespace(t, shape.name_space);
        t.expect_all({"pub", "async", "routine", "main", "(", ")", ":", "void"});
        t.body_only();
    }
    if (shape.state_field.empty() || shape.contract_method.empty() ||
        !shape.external_receive_runtime_only)
        throw std::runtime_error("ACTOR_DECL_HEADER_REJECT: incomplete declaration shape");
    return shape; // No source-body typecheck, no state loans, no actor IR.
}

} // namespace oreslang::frontend
