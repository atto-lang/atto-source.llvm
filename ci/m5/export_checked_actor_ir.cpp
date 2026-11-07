// Experimental M5 projection of the *canonical LLVM frontend's checked AST*.
// This is a narrow, fail-closed typed actor IR export for backend interoperability,
// NOT a second AttoLang parser and NOT a full canonical HIR/MIR serialization.
// Does not statically admit async/await, rt take/share, critical state or other
// operations until native type+ownership validation and projection are complete.
#include "oreslang/frontend/Parser.hpp"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using oreslang::ActorAction;
using oreslang::ActorDecl;
using oreslang::Expression;
using oreslang::Function;
using oreslang::Program;

static void require(bool p, const char* why) {
    if (!p) throw std::runtime_error(why);
}
static std::string escaped(const std::string& s) {
    std::string o = "\"";
    static constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(static_cast<char>(c)); }
        else if (c < 0x20u) { o += "\\u00"; o.push_back(digits[c >> 4u]); o.push_back(digits[c & 0x0fu]); }
        else o.push_back(static_cast<char>(c));
    }
    return o + "\"";
}
static void identifier(const std::string& s) {
    require(!s.empty() && s.size() <= 128u, "M5_MIR_UNSUPPORTED: invalid identifier length");
    for (unsigned char c : s)
        require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_',
                "M5_MIR_UNSUPPORTED: non-ASCII identifier");
}
static void variable(const Expression* e, const std::string& name) {
    require(e && e->kind == Expression::Kind::Variable && e->name == name,
            "M5_MIR_UNSUPPORTED: actor expression must use checked scalar variable");
}
static std::int64_t integer(const Expression* e) {
    require(e && e->kind == Expression::Kind::Integer,
            "M5_MIR_UNSUPPORTED: only literal i64 payload/initializer supported");
    return e->integer;
}
static void checked_actor(const ActorDecl& actor) {
    identifier(actor.name);
    require(actor.fields.size() == 1 && actor.on_message.parameters.size() == 1 &&
                actor.on_message.actor_field_writes.size() == 1 && actor.on_message.result,
            "M5_MIR_UNSUPPORTED: only one-field additive actor handler supported");
    const auto& field = actor.fields.front();
    const auto& parameter = actor.on_message.parameters.front();
    identifier(field.name);
    identifier(parameter.name);
    require(field.name != parameter.name && !field.heap_owned && !field.int_array &&
                !field.new_class && !field.field_mutable /* validated state is private */,
            "M5_MIR_UNSUPPORTED: unsafe actor field ownership or alias");
    require(field.scalar_type == oreslang::ScalarType::I64 &&
                parameter.scalar_type == oreslang::ScalarType::I64,
            "M5_MIR_UNSUPPORTED: actor payload/field must be i64");
    (void)integer(field.initializer.get());
    const auto& write = actor.on_message.actor_field_writes.front();
    require(write.field == field.name && write.expression &&
                write.expression->kind == Expression::Kind::Add,
            "M5_MIR_UNSUPPORTED: actor handler requires checked i64 field += parameter");
    variable(write.expression->left.get(), field.name);
    variable(write.expression->right.get(), parameter.name);
    variable(actor.on_message.result.get(), field.name);
    require(actor.on_message.actor_sends.empty() && actor.on_message.actor_spawns.empty() &&
                actor.on_message.actor_stops.empty() && actor.on_message.locals.empty() &&
                actor.on_message.statements.empty() && actor.on_message.early_returns.empty(),
            "M5_MIR_UNSUPPORTED: actor handler side effects outside checked expression");
}
static void project(const Program& p) {
    require(p.imports.empty() && p.classes.empty() && p.actors.size() == 1 &&
                p.functions.size() == 1 && p.functions.front().name == "main",
            "M5_MIR_UNSUPPORTED: requires one actor and one standalone main");
    const auto& actor = p.actors.front();
    checked_actor(actor);
    const auto& fn = p.functions.front();
    require(fn.parameters.empty() && fn.actor_spawns.size() <= 64u &&
                fn.actor_sends.size() <= 1024u && fn.actor_actions.size() <= 2048u &&
                fn.actor_actions.size() == fn.actor_sends.size() + fn.actor_stops.size() &&
                fn.result && fn.result->kind == Expression::Kind::Integer &&
                fn.locals.empty() && fn.statements.empty() && fn.early_returns.empty() &&
                fn.source_tasks.empty() && fn.future_pipelines.empty() &&
                fn.actor_field_writes.empty() && !fn.scalar_while,
            "M5_MIR_UNSUPPORTED: unsupported main effects/types");
    const auto& fld = actor.fields.front();
    const auto& param = actor.on_message.parameters.front();
    std::cout << "{\"schema\":\"atto.canonical-checked-actor-ir.v0\","
              << "\"checker\":\"LLVM_PARSE_PROGRAM_OWNERSHIP_SCALAR_TYPES\","
              << "\"status\":\"CHECKED_RESTRICTED_ACTOR_SLICE\","
              << "\"actor\":{\"name\":" << escaped(actor.name)
              << ",\"field\":" << escaped(fld.name)
              << ",\"initial\":" << integer(fld.initializer.get())
              << ",\"parameter\":" << escaped(param.name)
              << ",\"operation\":\"checked_add_i64_return_field\"},"
              << "\"spawns\":[";
    for (std::size_t i = 0; i < fn.actor_spawns.size(); ++i) {
        const auto& spawn = fn.actor_spawns[i];
        identifier(spawn.name); identifier(spawn.actor_type);
        require(spawn.actor_type == actor.name, "M5_MIR_UNSUPPORTED: unknown actor kind");
        if (i) std::cout << ',';
        std::cout << "{\"ref\":" << escaped(spawn.name) << ",\"kind\":"
                  << escaped(spawn.actor_type) << "}";
    }
    std::cout << "],\"actions\":[";
    for (std::size_t i = 0; i < fn.actor_actions.size(); ++i) {
        const auto& action = fn.actor_actions[i];
        if (i) std::cout << ',';
        if (action.kind == ActorAction::Kind::Send) {
            require(action.index < fn.actor_sends.size(),
                    "M5_MIR_UNSUPPORTED: checked actor send index invalid");
            const auto& send = fn.actor_sends[action.index];
            identifier(send.reference);
            std::cout << "{\"op\":\"send\",\"ref\":" << escaped(send.reference)
                      << ",\"value\":" << integer(send.payload.get()) << '}';
        } else {
            require(action.index < fn.actor_stops.size(),
                    "M5_MIR_UNSUPPORTED: checked actor stop index invalid");
            const auto& name = fn.actor_stops[action.index];
            identifier(name);
            std::cout << "{\"op\":\"stop\",\"ref\":" << escaped(name) << '}';
        }
    }
    std::cout << "],\"return_i64\":" << fn.result->integer
              << ",\"limits\":{\"max_actor_instances\":64,\"max_source_sends\":1024},"
              << "\"unsupported\":[\"await\",\"critical-read\",\"critical-write\","
              << "\"rt_take\",\"rt_share\",\"dynamic_guest_code\",\"tenant_sandbox\"]}\n";
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "M5_MIR_REFUSED: exactly one source path required");
        std::ifstream source(argv[1], std::ios::binary | std::ios::ate);
        require(source.good(), "M5_MIR_REFUSED: cannot open source");
        const auto size = source.tellg();
        require(size > 0 && size <= 32768, "M5_MIR_REFUSED: source byte budget");
        source.seekg(0);
        std::string text(static_cast<std::size_t>(size), '\0');
        source.read(text.data(), size);
        require(source.good() && !source.fail(), "M5_MIR_REFUSED: source read incomplete");
        // This is the actual existing LLVM frontend, including its native
        // ownership/type checks, not a regex or replacement parser.
        Program p = oreslang::parse_program(text);
        project(p);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "M5_CANONICAL_ACTOR_IR_REFUSED: " << e.what() << '\n';
        return 2;
    }
}
