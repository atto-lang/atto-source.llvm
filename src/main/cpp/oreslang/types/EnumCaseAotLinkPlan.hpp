#pragma once
// Closed-world, host-only enum-case direct-call linker proof using the
// canonical eos2 slot ABI from EnumCaseOverloadPolicy. NOT guest source codegen.
// Only MODULE/STATIC are direct-linked. INSTANCE needs an exact root vtable slot.
#include "oreslang/types/EnumCaseOverloadPolicy.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types::case_aot {

inline bool sha256Hex(const std::string& value) {
    if (value.size() != 64) return false;
    for (char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

class Image final {
public:
    class DirectCall final {
    public:
        const std::string& symbol() const { return symbol_; }
        const std::string& bodyDigest() const { return bodyDigest_; }
    private:
        DirectCall(std::uint64_t origin, std::string symbol, std::string digest)
            : origin_(origin), symbol_(std::move(symbol)),
              bodyDigest_(std::move(digest)) {}
        std::uint64_t origin_;
        const std::string symbol_;
        const std::string bodyDigest_;
        friend class Image;
    };

    Image(case_overloads::Registry registry,
          std::vector<case_overloads::Signature> declarations,
          std::map<std::string, std::string> compiledBodyDigests)
        : registry_(std::move(registry)),
          declarations_(std::move(declarations)),
          compiled_(std::move(compiledBodyDigests)),
          imageId_(nextIdentity()) {
        case_overloads::checkFamily(declarations_, registry_);
        if (declarations_.front().domain == case_overloads::Domain::Instance)
            throw std::invalid_argument("virtual instance method requires exact vtable slot");
        if (compiled_.size() != declarations_.size())
            throw std::invalid_argument("missing or extraneous AOT implementation");
        for (const auto& slot : declarations_) {
            const std::string symbol = slot.symbolId();
            const auto compiled = compiled_.find(symbol);
            if (compiled == compiled_.end())
                throw std::invalid_argument("missing exact eos2 AOT implementation");
            if (!sha256Hex(compiled->second))
                throw std::invalid_argument("noncanonical compiled body fingerprint");
        }
        // No arbitrary string name/arity dispatcher is stored in this image.
    }

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&&) = delete;
    Image& operator=(Image&&) = delete;

    DirectCall bind(const std::vector<case_overloads::Parameter>& staticTypes) const {
        const std::size_t selected =
            case_overloads::selectStatic(declarations_, registry_, staticTypes);
        const std::string exact = declarations_[selected].symbolId();
        const auto implementation = compiled_.find(exact);
        if (implementation == compiled_.end())
            throw std::invalid_argument("AOT missing selected direct-call body");
        return DirectCall(imageId_, exact, implementation->second);
    }

    void verify(const DirectCall& call) const {
        const auto registered = compiled_.find(call.symbol_);
        if (call.origin_ != imageId_ || registered == compiled_.end()
            || registered->second != call.bodyDigest_) {
            throw std::invalid_argument("stale or invalid enum-case direct-call relocation");
        }
    }

    std::size_t definitionCount() const { return compiled_.size(); }

private:
    static std::uint64_t nextIdentity() {
        static std::atomic<std::uint64_t> seq{1};
        const std::uint64_t value = seq.fetch_add(1, std::memory_order_relaxed);
        if (value == 0)
            throw std::invalid_argument("AOT image identity exhausted");
        return value;
    }

    const case_overloads::Registry registry_;
    const std::vector<case_overloads::Signature> declarations_;
    const std::map<std::string, std::string> compiled_;
    const std::uint64_t imageId_;
};

} // namespace oreslang::types::case_aot
