#pragma once

#include "oreslang/runtime/SymbolPropertyLayoutV1.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::symbol_image_v1 {

using symbol_property_v1::Domain;
using symbol_property_v1::Key;
using symbol_property_v1::KeyLess;

/**
 * HOST-ONLY C++17 immutable literal image foundation.
 *
 * This is NOT native AttoLang #Symbol lowering, guest actor admission, OS
 * mprotect, physical native RO pages, NFC Unicode parity, or an untrusted
 * capability interface. Native property-key policy currently admits ASCII
 * PROCESS names and canonical lowercase UUID STABLE names only.
 *
 * VM-owned strong pins retain an immutable generation after replacement
 * or store shutdown. Never send shared_ptr, ordinal or an address to guests.
 */
class SharedSymbolLiteralImageStore {
public:
    static constexpr std::size_t MAX_LITERALS = 4096;
    static constexpr std::size_t MAX_PAYLOAD_BYTES = 1048576;
    static constexpr std::size_t MAX_LIVE_GENERATIONS = 8;

    class Image final {
    public:
        std::uint64_t generation() const noexcept { return generation_; }
        std::size_t size() const noexcept { return values_.size(); }
        std::size_t payload_bytes() const noexcept { return payload_bytes_; }

        // Return an owned nominal value: returning a reference here would
        // dangle for expressions such as store.pin_latest()->at(0), once
        // the temporary pin is destroyed.
        Key at(std::size_t ordinal) const { return values_.at(ordinal); }

        std::size_t require_ordinal(const Key& logical) const {
            auto found = ordinals_.find(logical);
            if (found == ordinals_.end()) throw std::out_of_range("unknown Symbol literal");
            return found->second;
        }

        // v1 core manifest only; no tenant-scoped userland exports.
        // Returns a separately owned logical Symbol, not the image's storage.
        Key copy_for_untrusted_core(std::size_t ordinal) const {
            const auto& value = at(ordinal);
            if (value.domain() != Domain::ProcessSymbol ||
                    (value.name() != "ready" && value.name() != "error"
                     && value.name() != "Symbol.iterator")) {
                throw std::invalid_argument("Symbol not admitted to untrusted core");
            }
            return Key::process_symbol(value.name());
        }

    private:
        friend class SharedSymbolLiteralImageStore;
        Image(std::uint64_t generation, std::vector<Key> values,
              std::map<Key, std::size_t, KeyLess> ordinals,
              std::size_t payload_bytes)
            : generation_(generation), values_(std::move(values)),
              ordinals_(std::move(ordinals)), payload_bytes_(payload_bytes) {}

        const std::uint64_t generation_;
        const std::vector<Key> values_;
        const std::map<Key, std::size_t, KeyLess> ordinals_;
        const std::size_t payload_bytes_;
    };

    using Pin = std::shared_ptr<const Image>;

    // Fully stage outside the registry lock; rejected inputs never publish.
    std::uint64_t publish(const std::vector<Key>& input) {
        // A retired store must reject before copying potentially large host
        // vectors or interpreting their contents. Still recheck under the
        // commit lock after staging: close may race during allocation.
        {
            std::lock_guard<std::mutex> preflight(lock_);
            if (closed_) throw std::logic_error("Symbol image store is closed");
        }
        if (input.size() > MAX_LITERALS)
            throw std::invalid_argument("Symbol image literal-count quota exceeded");

        std::vector<Key> values;
        values.reserve(input.size());
        std::map<Key, std::size_t, KeyLess> positions;
        std::size_t bytes = 0;
        for (const auto& literal : input) {
            if (literal.domain() == Domain::String)
                throw std::invalid_argument("String property is not a Symbol literal");
            if (literal.name().size() > MAX_PAYLOAD_BYTES - bytes)
                throw std::invalid_argument("Symbol literal image byte budget exceeded");
            if (!positions.emplace(literal, values.size()).second)
                throw std::invalid_argument("duplicate canonical Symbol literal");
            bytes += literal.name().size();
            values.push_back(literal);
        }

        std::lock_guard<std::mutex> guard(lock_);
        if (closed_) throw std::logic_error("Symbol image store is closed");
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                     [](const auto& old) { return old.expired(); }),
                       retired_.end());

        const bool keep_old = current_ && current_.use_count() > 1;
        const std::size_t new_live = retired_.size() + (keep_old ? 1u : 0u) + 1u;
        if (new_live > MAX_LIVE_GENERATIONS)
            throw std::logic_error("too many live Symbol image generations");
        // Reserve/allocate before changing current, so failure cannot expose
        // a partially staged generation.
        if (keep_old) retired_.reserve(retired_.size() + 1);
        // Store-scoped sequence counters would allow a generation number from
        // an unrelated store to match by accident (cross-store ABA).
        // Allocate unique VM-local epochs, never exposed as capabilities.
        std::uint64_t candidate = next_global_generation_.load();
        for (;;) {
            if (candidate == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("Symbol image generation exhausted");
            if (next_global_generation_.compare_exchange_weak(candidate, candidate + 1)) break;
        }
        auto sealed = std::shared_ptr<const Image>(new Image(
            candidate, std::move(values), std::move(positions), bytes));

        Pin previous = current_;
        current_ = std::move(sealed);
        if (keep_old) retired_.push_back(previous);
        return current_->generation();
    }

    Pin pin_latest() const {
        std::lock_guard<std::mutex> guard(lock_);
        if (closed_ || !current_) throw std::logic_error("no active Symbol image");
        return current_;
    }

    Pin pin_expected(std::uint64_t expected) const {
        std::lock_guard<std::mutex> guard(lock_);
        if (closed_ || !current_ || current_->generation() != expected)
            throw std::logic_error("stale Symbol image generation");
        return current_;
    }

    std::size_t live_generation_count() const {
        std::lock_guard<std::mutex> guard(lock_);
        std::size_t count = current_ ? 1u : 0u;
        for (const auto& old : retired_) if (!old.expired()) ++count;
        return count;
    }

    void close() {
        std::lock_guard<std::mutex> guard(lock_);
        if (closed_) return;
        // Keep weak observational metadata for pinned images after closing.
        // Otherwise live_generation_count() would report zero while callers
        // still hold real memory. Strong data lifetime remains pin-owned.
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                     [](const auto& old) { return old.expired(); }),
                       retired_.end());
        if (current_ && current_.use_count() > 1)
            retired_.push_back(current_);
        closed_ = true;
        current_.reset();
    }

private:
    mutable std::mutex lock_;
    Pin current_;
    std::vector<std::weak_ptr<const Image>> retired_;
    inline static std::atomic<std::uint64_t> next_global_generation_{1};
    bool closed_ = false;
};

} // namespace oreslang::symbol_image_v1
