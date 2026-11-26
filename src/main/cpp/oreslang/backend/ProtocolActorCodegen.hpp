#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/VerifiedProtocol.hpp"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SHA256.h"

#include <map>
#include <vector>

namespace oreslang::protocol_actor_codegen {

inline void write_little_endian(std::vector<std::uint8_t>& bytes,
        std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

inline void write_string(std::vector<std::uint8_t>& bytes, llvm::StringRef value) {
    auto length = value.size();
    while (length >= 128) {
        bytes.push_back(static_cast<std::uint8_t>((length & 127) | 128));
        length >>= 7;
    }
    bytes.push_back(static_cast<std::uint8_t>(length));
    bytes.insert(bytes.end(), value.bytes_begin(), value.bytes_end());
}

inline void write_decimal(std::vector<std::uint8_t>& bytes, const llvm::APInt& value) {
    llvm::SmallString<80> decimal;
    value.toString(decimal, 10, false);
    write_string(bytes, decimal);
}

struct Layout {
    std::map<std::string, std::uint16_t> states;
    std::map<std::string, const std::vector<ProtocolField>*> messages;
    std::map<std::pair<std::string, std::string>, std::string> edges;

    explicit Layout(const VerifiedProtocol& protocol) {
        states.emplace(protocol.initial_state, 0);
        for (const auto& terminal : protocol.terminal_states) states.emplace(terminal, 0);
        for (const auto& edge : protocol.edges) {
            states.emplace(edge.source, 0);
            states.emplace(edge.target, 0);
            messages.emplace(edge.message, &edge.payload);
            edges.emplace(std::make_pair(edge.source, edge.message), edge.target);
        }
        std::uint16_t ordinal = 0;
        for (auto& state : states) state.second = ordinal++;
    }

    std::vector<std::uint8_t> descriptor(const VerifiedProtocolActor& actor) const {
        const auto& protocol = *actor.protocol;
        std::vector<std::uint8_t> canonical;
        write_string(canonical, "atto.protocol.mailbox.v1");
        write_string(canonical, protocol.name);
        write_string(canonical, protocol.initial_state);
        write_little_endian(canonical, states.size(), 4);
        for (const auto& state : states) {
            write_string(canonical, state.first);
            canonical.push_back(protocol.terminal_states.count(state.first) != 0);
        }

        write_little_endian(canonical, messages.size(), 4);
        for (const auto& [name, fields] : messages) {
            write_string(canonical, name);
            write_little_endian(canonical, fields->size(), 4);
            for (const auto& field : *fields) {
                const auto& type = field.type;
                canonical.push_back(type.i32);
                write_little_endian(canonical, type.minimum, 8);
                write_little_endian(canonical, type.maximum, 8);
                write_little_endian(canonical, type.unit.powers().size(), 4);
                for (const auto& [dimension, exponent] : type.unit.powers()) {
                    write_string(canonical, dimension);
                    write_little_endian(canonical, exponent, 4);
                }
                write_decimal(canonical, type.unit.numerator());
                write_decimal(canonical, type.unit.denominator());
            }
        }

        write_little_endian(canonical, edges.size(), 4);
        for (const auto& [key, destination] : edges) {
            write_string(canonical, key.first);
            write_string(canonical, key.second);
            write_string(canonical, destination);
        }
        const auto digest = llvm::SHA256::hash(canonical);
        std::vector<std::uint8_t> plan(digest.begin(), digest.end());
        write_little_endian(plan, states.at(protocol.initial_state), 2);
        write_little_endian(plan, states.size(), 2);
        write_little_endian(plan, messages.size(), 2);
        write_little_endian(plan, edges.size(), 2);

        std::map<std::string, std::uint16_t> tags;
        for (const auto& [name, fields] : messages) {
            tags.emplace(name, static_cast<std::uint16_t>(tags.size()));
            write_little_endian(plan, fields->size(), 2);
            for (const auto& field : *fields) {
                write_little_endian(plan, field.type.minimum, 8);
                write_little_endian(plan, field.type.maximum, 8);
            }
        }
        for (const auto& [key, destination] : edges) {
            write_little_endian(plan, states.at(key.first), 2);
            write_little_endian(plan, tags.at(key.second), 2);
            write_little_endian(plan, states.at(destination), 2);
        }
        if (actor.field) {
            const auto& field = *actor.field;
            write_little_endian(plan, field.initial, 8);
            write_little_endian(plan, field.field.type.minimum, 8);
            write_little_endian(plan, field.field.type.maximum, 8);
            for (const auto& entry : messages) {
                const auto handler = std::find_if(actor.handlers.begin(), actor.handlers.end(),
                    [&](const auto& candidate) { return candidate.message == entry.first; });
                write_little_endian(plan, handler->increment_parameter.value_or(UINT16_MAX), 2);
            }
        }
        return plan;
    }
};

inline std::string emit(const Program& program) {
    llvm::LLVMContext context;
    llvm::Module module("atto.verified.protocol.actors", context);
    llvm::IRBuilder<> ir(context);
    auto* i64 = ir.getInt64Ty();
    auto* ptr = ir.getPtrTy();
    const auto spawn_runtime = module.getOrInsertFunction(
        "oreslang_source_actor_spawn_protocol_id", i64, ptr, i64);
    auto* i32 = ir.getInt32Ty();
    const auto send_runtime = module.getOrInsertFunction(
        "oreslang_source_actor_try_send_protocol_id", i32, i64, ptr, i64);
    const auto encode_runtime = module.getOrInsertFunction(
        "oreslang_source_actor_encode_protocol_data", i64, ptr, i64, ptr, i64, ptr, i64);

    for (const auto& actor : program.protocol_actors) {
        const Layout layout(*actor->protocol);
        const auto plan = layout.descriptor(*actor);
        auto* contents = llvm::ConstantDataArray::get(context, plan);
        auto* descriptor = new llvm::GlobalVariable(module, contents->getType(), true,
            llvm::GlobalValue::PrivateLinkage, contents, "plan." + actor->name);
        auto* spawn = llvm::Function::Create(llvm::FunctionType::get(i64, false),
            llvm::GlobalValue::ExternalLinkage, "oreslang_protocol_spawn_" + actor->name, module);
        ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", spawn));
        ir.CreateRet(ir.CreateCall(spawn_runtime, {descriptor, ir.getInt64(plan.size())}));

        std::uint16_t tag = 0;
        // The actor-name length keeps actor/message pairs injective.
        const auto suffix = std::to_string(actor->name.size()) + "_" + actor->name + "_";
        for (const auto& [name, fields] : layout.messages) {
            std::vector<llvm::Type*> encode_parameters{ptr, i64};
            std::vector<llvm::Type*> send_parameters{i64};
            for (const auto& field : *fields) {
                auto* type = field.type.i32 ? i32 : i64;
                encode_parameters.push_back(type);
                send_parameters.push_back(type);
            }
            auto* encode = llvm::Function::Create(llvm::FunctionType::get(i64, encode_parameters, false),
                llvm::GlobalValue::ExternalLinkage, "oreslang_protocol_encode_" + suffix + name, module);
            ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", encode));
            llvm::Value* values = llvm::ConstantPointerNull::get(ptr);
            if (!fields->empty()) values = ir.CreateAlloca(i64, ir.getInt64(fields->size()));
            for (std::size_t i = 0; i < fields->size(); ++i) {
                auto* argument = encode->getArg(i + 2);
                auto* value = ir.CreateSExtOrTrunc(argument, i64);
                ir.CreateStore(value, ir.CreateInBoundsGEP(i64, values, ir.getInt64(i)));
            }
            ir.CreateRet(ir.CreateCall(encode_runtime, {
                descriptor, ir.getInt64(tag++), values, ir.getInt64(fields->size()),
                encode->getArg(0), encode->getArg(1)
            }));

            auto* send = llvm::Function::Create(llvm::FunctionType::get(i32, send_parameters, false),
                llvm::GlobalValue::ExternalLinkage, "oreslang_protocol_send_" + suffix + name, module);
            ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", send));
            auto* capacity = ir.getInt64(39 + 8 * fields->size());
            auto* frame = ir.CreateAlloca(ir.getInt8Ty(), capacity);
            std::vector<llvm::Value*> encode_args{frame, capacity};
            for (std::size_t i = 0; i < fields->size(); ++i)
                encode_args.push_back(send->getArg(i + 1));
            auto* length = ir.CreateCall(encode, encode_args);
            ir.CreateRet(ir.CreateCall(send_runtime, {send->getArg(0), frame, length}));
        }
    }
    if (program.protocol_guest) {
        const auto& guest = *program.protocol_guest;
        const auto complete = module.getOrInsertFunction(
            "oreslang_source_actor_complete_protocol_send_id", i32, i64, i32);
        const auto stop = module.getOrInsertFunction(
            "oreslang_source_actor_stop_id", ir.getVoidTy(), i64);
        auto* main = llvm::Function::Create(llvm::FunctionType::get(i32, false),
            llvm::GlobalValue::ExternalLinkage, "main", module);
        ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", main));
        auto* actor = ir.CreateCall(module.getFunction("oreslang_protocol_spawn_" + guest.actor));
        llvm::Value* result = nullptr;
        const auto prefix = "oreslang_protocol_send_" + std::to_string(guest.actor.size()) +
            "_" + guest.actor + "_";
        for (std::size_t i = 0; i < guest.turns.size(); ++i) {
            const auto& turn = guest.turns[i];
            auto* send = module.getFunction(prefix + turn.message);
            std::vector<llvm::Value*> arguments{actor};
            for (std::size_t j = 0; j < turn.payload.size(); ++j)
                arguments.push_back(llvm::ConstantInt::get(send->getArg(j + 1)->getType(), turn.payload[j], true));
            auto* admitted = ir.CreateCall(send, arguments);
            auto* status = ir.CreateCall(complete, {actor, admitted});
            if (i == guest.result) result = status;
        }
        ir.CreateCall(stop, {actor});
        ir.CreateRet(result);
    }
    std::string output;
    llvm::raw_string_ostream stream(output);
    if (llvm::verifyModule(module, &stream))
        throw std::runtime_error("E_PROTOCOL_CODEGEN: " + output);
    module.print(stream, nullptr);
    return output;
}

} // namespace oreslang::protocol_actor_codegen
