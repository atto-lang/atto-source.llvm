#include "oreslang/backend/NativeSourceFieldWriteLinks.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace oreslang::native_ir;

void check(bool good, const char* name, unsigned& count) {
    ++count;
    if (!good) throw std::runtime_error(std::string("case ") +
        std::to_string(count) + ": " + name);
}
bool denied(const std::function<void()>& action) {
    try { action(); }
    catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:") !=
            std::string::npos;
    }
    return false;
}

int main() {
    unsigned count = 0;
    try {
        llvm::LLVMContext context;
        llvm::Module module("field-source-emission", context);
        auto* i64 = llvm::Type::getInt64Ty(context);
        auto* void_type = llvm::Type::getVoidTy(context);
        auto* fn = llvm::Function::Create(
            llvm::FunctionType::get(void_type, {i64}, false),
            llvm::Function::ExternalLinkage, "guest", module);
        auto* block = llvm::BasicBlock::Create(context, "entry", fn);
        llvm::IRBuilder<> builder(block);
        auto* arg = fn->getArg(0);
        auto* slot = builder.CreateAlloca(i64, nullptr, "p.x");
        auto* store = builder.CreateStore(arg, slot);
        builder.CreateRetVoid();

        check(!llvm::verifyModule(module, &llvm::errs()),
            "test fixture is verified LLVM IR", count);
        const auto physical = collect_native_ir_ssa_census(module);
        CompilerFieldWriteLinks links;
        check(links.sites().empty(), "no caller-fabricated source site", count);
        check(links.inspect(module, physical).physically_matched_writes == 0,
            "empty source link inventory remains empty", count);
        check(!links.inspect(module, physical).source_ownership_complete,
            "matching source link never grants ownership proof", count);
        check(!links.inspect(module, physical).guest_verified,
            "source link never claims compiled guest parity", count);

        links.record(*fn, "p.x", SourceFieldWriteKind::Direct, *store);
        check(links.sites().size() == 1 &&
                links.sites().front().emission_site == 1,
            "compiler mints nonzero monotone emission ticket", count);
        check(links.sites().front().place == "p.x",
            "source projection recorded from codegen", count);
        check(links.sites().front().kind == SourceFieldWriteKind::Direct,
            "direct field write origin preserved", count);
        check(links.inspect(module, physical).physically_matched_writes == 1,
            "physical StoreInst and source site match", count);

        check(denied([&] {
            links.record(*fn, "", SourceFieldWriteKind::Direct, *store);
        }), "empty projection rejected", count);
        check(denied([&] {
            links.record(*fn, ".x", SourceFieldWriteKind::Direct, *store);
        }), "leading dot rejected", count);
        check(denied([&] {
            links.record(*fn, "p.", SourceFieldWriteKind::Direct, *store);
        }), "trailing dot rejected", count);
        check(denied([&] {
            links.record(*fn, "p..x", SourceFieldWriteKind::Direct, *store);
        }), "noncanonical dotted projection rejected", count);
        check(denied([&] {
            links.record(*fn, std::string(257, 'q'),
                SourceFieldWriteKind::Direct, *store);
        }), "oversized field projection rejected", count);

        auto* other = llvm::Function::Create(
            llvm::FunctionType::get(void_type, false),
            llvm::Function::ExternalLinkage, "other", module);
        llvm::IRBuilder<> other_builder(
            llvm::BasicBlock::Create(context, "entry", other));
        other_builder.CreateRetVoid();
        check(denied([&] {
            links.record(*other, "p.x", SourceFieldWriteKind::Direct, *store);
        }), "cannot attribute another function's StoreInst", count);
        check(denied([&] {
            links.record(*fn, "p.x", SourceFieldWriteKind::Direct, *store);
            (void)links.inspect(module, collect_native_ir_ssa_census(module));
        }), "duplicate source field write claim for one physical store refused", count);

        auto fresh = collect_native_ir_ssa_census(module);
        auto* changed = llvm::ConstantInt::get(i64, 42);
        store->setOperand(0, changed);
        check(denied([&] {
            (void)links.inspect(module, fresh);
        }), "mutated store value invalidates previously captured source link", count);
        CompilerFieldWriteLinks updated;
        updated.record(*fn, "p.x", SourceFieldWriteKind::CatchCleanup, *store);
        const auto mutated = collect_native_ir_ssa_census(module);
        check(updated.inspect(module, mutated).physically_matched_writes == 1,
            "fresh source link matches actual changed emitted instruction", count);
        check(updated.sites().front().kind == SourceFieldWriteKind::CatchCleanup,
            "exception cleanup source kind retained", count);

        llvm::Module foreign("unrelated", context);
        check(denied([&] {
            (void)updated.inspect(foreign, mutated);
        }), "foreign module cannot authenticate source writes", count);

        llvm::IRBuilder<> before(block,
            block->getTerminator()->getIterator());
        auto* second = before.CreateStore(arg, slot);
        check(denied([&] {
            (void)updated.inspect(module, mutated);
        }), "added LLVM instruction invalidates old source-to-store snapshot", count);
        auto rederived = collect_native_ir_ssa_census(module);
        CompilerFieldWriteLinks two;
        two.record(*fn, "p.x", SourceFieldWriteKind::Direct, *store);
        two.record(*fn, "p.x", SourceFieldWriteKind::Deferred, *second);
        check(two.sites().back().emission_site == 2,
            "second compiler emission site is unique", count);
        check(two.inspect(module, rederived).physically_matched_writes == 2,
            "two distinct physical LLVM stores match source sites", count);
        check(two.sites().back().kind == SourceFieldWriteKind::Deferred,
            "deferred source write type preserved", count);

        const llvm::StoreInst* erased = second;
        second->eraseFromParent();
        check(denied([&] {
            (void)two.inspect(module, rederived);
        }), "deleted LLVM store fails without dereferencing erased pointer", count);
        check(erased != nullptr,
            "erased pointer remains opaque and never dereferenced", count);

        if (count != 26)
            throw std::runtime_error("wrong expected 26 LLVM source-store cases");
        std::cout << "PASS 26/26 compiler FieldWrite to real LLVM StoreInst "
                     "(HOST_LLVM_IR_ONLY; SOURCE_NLL_NOT_PROVEN)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED after case " << count << ": " << e.what() << '\n';
        return 1;
    }
}
