#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace oreslang::types {
// Candidate hosted inline ABI; not the existing region-handle sum ABI.
// A compiler must prove each owned leaf's size/alignment and destructor ID.
// No source admission, move lowering, or cross-target ABI is implied.
class NativeInlineSumLayout final {
public:
    static constexpr std::size_t max_nodes = 128;
    static constexpr std::size_t max_depth = 32;
    static constexpr std::size_t max_bytes = 1024 * 1024;
    enum class Kind : std::uint8_t { I64, Bool, NumericError, OwnedLeaf, Option, Result };
    struct Node {
        Kind kind;
        std::size_t first = 0, second = 0;
        std::size_t bytes = 0, alignment = 0;
        std::uint32_t destructor_id = 0;
    };
    struct Entry {
        Node node{};
        std::size_t bytes = 0, alignment = 0, payload_offset = 0, depth = 0;
        bool needs_drop = false;
    };
    enum class Retire { Retired, Inactive, Invalid };
    using Drop = void (*)(void*, std::uint32_t, unsigned char*) noexcept;

    // Postorder type DAG: child indices must refer to earlier entries.
    // Shared type descriptors are permitted; recursive by-value cycles aren't.
    static NativeInlineSumLayout build(const Node* nodes, std::size_t count) {
        if (!nodes || count == 0 || count > max_nodes)
            throw std::invalid_argument("invalid inline sum node count");
        NativeInlineSumLayout out;
        for (std::size_t i = 0; i < count; ++i) {
            const Node n = nodes[i];
            Entry e{n, 0, 0, 0, 1, false};
            switch (n.kind) {
                case Kind::I64: case Kind::Bool: case Kind::NumericError:
                    if (n.first || n.second || n.bytes || n.alignment || n.destructor_id)
                        throw std::invalid_argument("noncanonical scalar layout");
                    e.bytes = e.alignment = n.kind == Kind::I64 ? 8 : n.kind == Kind::Bool ? 1 : 4;
                    break;
                case Kind::OwnedLeaf:
                    if (n.first || n.second || !n.destructor_id || !n.bytes ||
                            !valid_alignment(n.alignment) || n.bytes > max_bytes || n.bytes % n.alignment)
                        throw std::invalid_argument("unproved owned leaf layout");
                    e.bytes = n.bytes; e.alignment = n.alignment; e.needs_drop = true;
                    break;
                case Kind::Option: case Kind::Result: {
                    if (n.first >= i || n.bytes || n.alignment || n.destructor_id ||
                            (n.kind == Kind::Option ? n.second != 0 : n.second >= i))
                        throw std::invalid_argument("invalid inline sum child or metadata");
                    const auto& first = out.entries_[n.first];
                    const Entry empty{};
                    const auto& second = n.kind == Kind::Result ? out.entries_[n.second] : empty;
                    e.alignment = first.alignment > second.alignment ? first.alignment : second.alignment;
                    e.depth = 1 + (first.depth > second.depth ? first.depth : second.depth);
                    e.needs_drop = first.needs_drop || second.needs_drop;
                    e.payload_offset = round_up(1, e.alignment);
                    const auto payload = first.bytes > second.bytes ? first.bytes : second.bytes;
                    if (payload > max_bytes - e.payload_offset)
                        throw std::invalid_argument("inline sum size exceeds budget");
                    e.bytes = round_up(e.payload_offset + payload, e.alignment);
                    break;
                }
                default: throw std::invalid_argument("unknown inline sum kind");
            }
            if (e.depth > max_depth || e.bytes > max_bytes)
                throw std::invalid_argument("inline sum exceeds layout budget");
            out.entries_[i] = e;
        }
        out.count_ = count;
        return out;
    }
    const Entry& root() const noexcept { return entries_[count_ - 1]; }
    const Entry& entry(std::size_t i) const {
        if (i >= count_) throw std::out_of_range("inline sum node index");
        return entries_[i];
    }
    // Caller owns a unique concrete live flag and initialized storage. Walk
    // only the active path (Option None=0, Some=1; Result Ok=0, Err=1).
    // Validate before any callback, and consume the live flag before dispatch.
    // No allocation or recursion on retirement; inactive bytes are never read.
    Retire retire(unsigned char* storage, std::size_t bytes, bool& live,
                  Drop drop, void* context) const noexcept {
        if (!live) return Retire::Inactive;
        if (!storage || bytes < root().bytes ||
                reinterpret_cast<std::uintptr_t>(storage) % root().alignment)
            return Retire::Invalid;
        std::size_t index = count_ - 1;
        unsigned char* active = storage;
        for (;;) {
            const Entry& e = entries_[index];
            if (e.node.kind != Kind::Option && e.node.kind != Kind::Result) {
                if (e.needs_drop && !drop) return Retire::Invalid;
                live = false;
                if (e.needs_drop) drop(context, e.node.destructor_id, active);
                return Retire::Retired;
            }
            const unsigned char tag = *active;
            if (tag > 1) return Retire::Invalid;
            if (e.node.kind == Kind::Option && tag == 0) {
                live = false;
                return Retire::Retired;
            }
            active += e.payload_offset;
            index = e.node.kind == Kind::Result && tag == 1 ? e.node.second : e.node.first;
        }
    }
private:
    NativeInlineSumLayout() = default;
    static bool valid_alignment(std::size_t a) noexcept {
        return a && a <= 64 && (a & (a - 1)) == 0;
    }
    static std::size_t round_up(std::size_t n, std::size_t a) {
        if (!valid_alignment(a) || n > max_bytes - (a - 1))
            throw std::invalid_argument("inline sum alignment exceeds budget");
        return (n + a - 1) & ~(a - 1);
    }
    std::array<Entry, max_nodes> entries_{};
    std::size_t count_ = 0;
};
} // namespace oreslang::types
