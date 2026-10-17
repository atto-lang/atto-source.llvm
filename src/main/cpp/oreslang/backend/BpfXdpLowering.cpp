#include "oreslang/backend/BpfXdpLowering.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#if LLVM_VERSION_MAJOR >= 21
#include "llvm/TargetParser/Triple.h"
#endif

namespace oreslang {
namespace {

struct PacketByteGuard {
    std::uint32_t offset = 0;
    std::uint8_t expected = 0;
    std::uint32_t action = 0;
};

bool basic_xdp_action(const Expression* expr, std::uint32_t& action) {
    if (!expr || expr->kind != Expression::Kind::Integer ||
        expr->integer < 0 || expr->integer > 3)
        return false;
    action = static_cast<std::uint32_t>(expr->integer);
    return true;
}

bool packet_u8(const Expression* expr, const std::string& context_name,
               std::uint32_t& offset) {
    if (!expr || expr->kind != Expression::Kind::ArrayCall ||
        expr->name != "packet_u8" || expr->arguments.size() != 1 ||
        !expr->left || expr->left->kind != Expression::Kind::Variable ||
        expr->left->name != context_name)
        return false;
    const auto* arg = expr->arguments.front().get();
    if (!arg || arg->kind != Expression::Kind::Integer ||
        arg->integer < 0 || arg->integer > 4095)
        return false;
    offset = static_cast<std::uint32_t>(arg->integer);
    return true;
}

PacketByteGuard parse_guard(const GuardedReturn& guard,
                            const std::string& context_name) {
    PacketByteGuard out;
    const Expression* packet = nullptr;
    const Expression* literal = nullptr;

    std::uint32_t ignored = 0;
    if (packet_u8(guard.lhs.get(), context_name, ignored) &&
        guard.rhs && guard.rhs->kind == Expression::Kind::Integer) {
        packet = guard.lhs.get();
        literal = guard.rhs.get();
    } else if (packet_u8(guard.rhs.get(), context_name, ignored) &&
               guard.lhs && guard.lhs->kind == Expression::Kind::Integer) {
        packet = guard.rhs.get();
        literal = guard.lhs.get();
    } else {
        throw std::runtime_error(
            "eBPF XDP guard requires ctx.packet_u8(const_offset) == byte literal");
    }

    if (literal->integer < 0 || literal->integer > 255)
        throw std::runtime_error("eBPF XDP packet_u8 comparison byte must be 0..255");
    if (!packet_u8(packet, context_name, out.offset))
        throw std::runtime_error("eBPF XDP packet_u8 offset must be a literal 0..4095");
    out.expected = static_cast<std::uint8_t>(literal->integer);
    if (!basic_xdp_action(guard.result.get(), out.action))
        throw std::runtime_error("eBPF XDP guarded return action must be a literal 0..3");
    return out;
}

llvm::Value* load_xdp_u32_field(llvm::IRBuilder<>& builder,
                                llvm::Value* context_ptr,
                                std::uint64_t byte_offset,
                                llvm::Type* i8,
                                llvm::Type* i32,
                                llvm::Type* i64) {
    auto* address = builder.CreateGEP(
        i8, context_ptr, llvm::ConstantInt::get(i64, byte_offset));
#if LLVM_VERSION_MAJOR < 18
    address = builder.CreateBitCast(
        address, llvm::PointerType::getUnqual(i32));
#endif
    auto* value = builder.CreateLoad(i32, address);
    value->setAlignment(llvm::Align(4));
    return value;
}

} // namespace

std::string emit_verified_bpf_xdp_ir(const Program& source) {
    if (!source.module.empty() || !source.classes.empty() ||
        !source.actors.empty() || !source.cooperative_loops.empty() ||
        source.functions.size() != 1)
        throw std::runtime_error("eBPF XDP requires one top-level exported ores_xdp function");

    const auto& fn = source.functions.front();
    if (fn.name != "ores_xdp" || !fn.exported ||
        !fn.generic_parameters.empty() || fn.returns_void ||
        !fn.result || fn.result->kind != Expression::Kind::Integer ||
        !fn.locals.empty() || !fn.statements.empty() ||
        !fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
        !fn.actor_stops.empty() || !fn.actor_actions.empty() ||
        !fn.actor_field_writes.empty() || fn.scalar_while ||
        fn.caught_throw_value || !fn.catch_binding.empty() ||
        !fn.caught_throw_defers.empty() || fn.has_declared_effects ||
        fn.declared_effects.bits != 0)
        throw std::runtime_error(
            "eBPF XDP admits only basic literal returns and checked packet_u8 guards");

    const bool constant_only = fn.parameters.empty();
    std::string context_name;
    if (!constant_only) {
        if (fn.parameters.size() != 1 ||
            fn.parameters.front().class_name != "XdpContext" ||
            !fn.parameters.front().generic_type.empty() ||
            fn.parameters.front().mode != Parameter::Mode::Borrow)
            throw std::runtime_error(
                "eBPF XDP packet access requires exactly one 'borrow XdpContext <name>' parameter");
        context_name = fn.parameters.front().name;
    }
    if (constant_only && !fn.early_returns.empty())
        throw std::runtime_error("eBPF XDP packet guards require borrow XdpContext");

    std::uint32_t fallback_action = 0;
    if (!basic_xdp_action(fn.result.get(), fallback_action))
        throw std::runtime_error("eBPF XDP return action must be an integer literal 0..3");

    std::vector<PacketByteGuard> guards;
    guards.reserve(fn.early_returns.size());
    for (const auto& guard : fn.early_returns)
        guards.push_back(parse_guard(guard, context_name));

    const auto profile = TargetAbi::parse("bpfel");
    if (!profile.experimental_bpf_ir() || profile.pointer_bits() != 64 ||
        profile.endian() != "little")
        throw std::runtime_error("eBPF XDP ABI profile mismatch");

    llvm::LLVMContext context;
    llvm::Module module("oreslang-bpf-xdp", context);
#if LLVM_VERSION_MAJOR >= 21
    module.setTargetTriple(llvm::Triple("bpfel"));
#else
    module.setTargetTriple("bpfel");
#endif
    module.setDataLayout("e-m:e-p:64:64-i64:64-i128:128-n32:64-S128");
    if (module.getDataLayout().getPointerSizeInBits(0) != 64 ||
        !module.getDataLayout().isLittleEndian())
        throw std::runtime_error("eBPF XDP LLVM data layout mismatch");

    auto* i8 = llvm::Type::getInt8Ty(context);
    auto* i32 = llvm::Type::getInt32Ty(context);
    auto* i64 = llvm::Type::getInt64Ty(context);
#if LLVM_VERSION_MAJOR >= 18
    auto* pointer = llvm::PointerType::get(context, 0);
#else
    auto* pointer = llvm::Type::getInt8PtrTy(context);
#endif
    auto* signature = llvm::FunctionType::get(i32, {pointer}, false);
    auto* xdp = llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                       "ores_xdp", module);
    xdp->addFnAttr(llvm::Attribute::NoUnwind);
    xdp->setSection("xdp");
    auto* kernel_context = xdp->getArg(0);
    kernel_context->setName("ctx");

    auto* entry = llvm::BasicBlock::Create(context, "entry", xdp);
    llvm::IRBuilder<> builder(entry);

    if (guards.empty()) {
        builder.CreateRet(llvm::ConstantInt::get(i32, fallback_action));
    } else {
        // Linux struct xdp_md starts with u32 data; u32 data_end. These are
        // verifier-tracked packet pointers. They never become Oreslang values.
        auto* data32 = load_xdp_u32_field(builder, kernel_context, 0, i8, i32, i64);
        auto* data_end32 = load_xdp_u32_field(builder, kernel_context, 4, i8, i32, i64);
        auto* data64 = builder.CreateZExt(data32, i64);
        auto* data_end64 = builder.CreateZExt(data_end32, i64);
        auto* data = builder.CreateIntToPtr(data64, pointer);
        auto* data_end = builder.CreateIntToPtr(data_end64, pointer);

        llvm::BasicBlock* current = entry;
        for (std::size_t index = 0; index < guards.size(); ++index) {
            const auto& guard = guards[index];
            if (builder.GetInsertBlock() != current)
                builder.SetInsertPoint(current);

            auto* byte_ptr = builder.CreateGEP(
                i8, data, llvm::ConstantInt::get(i64, guard.offset));
            auto* after_byte = builder.CreateGEP(
                i8, byte_ptr, llvm::ConstantInt::get(i64, 1));
            auto* in_bounds = builder.CreateICmpULE(after_byte, data_end);

            auto* load_block = llvm::BasicBlock::Create(
                context, "packet.load." + std::to_string(index), xdp);
            auto* next_block = llvm::BasicBlock::Create(
                context, "packet.next." + std::to_string(index), xdp);
            builder.CreateCondBr(in_bounds, load_block, next_block);

            builder.SetInsertPoint(load_block);
            auto* value = builder.CreateLoad(i8, byte_ptr);
            value->setAlignment(llvm::Align(1));
            auto* matches = builder.CreateICmpEQ(
                value, llvm::ConstantInt::get(i8, guard.expected));
            auto* action_block = llvm::BasicBlock::Create(
                context, "packet.action." + std::to_string(index), xdp);
            builder.CreateCondBr(matches, action_block, next_block);

            builder.SetInsertPoint(action_block);
            builder.CreateRet(llvm::ConstantInt::get(i32, guard.action));
            current = next_block;
        }
        builder.SetInsertPoint(current);
        builder.CreateRet(llvm::ConstantInt::get(i32, fallback_action));
    }

    // Stage 0 deliberately marks the object non-GPL. This is a kernel capability
    // restriction, not a legal license declaration: later helper/kfunc support
    // must add an explicit source/deployment licensing contract before GPL-only
    // capabilities can ever be admitted.
    auto* license_value = llvm::ConstantDataArray::getString(context, "Proprietary", true);
    auto* license = new llvm::GlobalVariable(
        module, license_value->getType(), true,
        llvm::GlobalValue::ExternalLinkage, license_value, "_license");
    license->setSection("license");

    std::string errors;
    llvm::raw_string_ostream diagnostics(errors);
    if (llvm::verifyModule(module, &diagnostics))
        throw std::runtime_error("eBPF LLVM IR verifier: " + diagnostics.str());
    std::string emitted;
    llvm::raw_string_ostream out(emitted);
    module.print(out, nullptr);
    out.flush();
    return emitted;
}

} // namespace oreslang
