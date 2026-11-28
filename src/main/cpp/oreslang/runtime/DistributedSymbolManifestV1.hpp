#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::distributed_symbol_v1 {
using Uuid = std::array<std::uint8_t, 16>;
using Declaration = std::pair<std::string, std::string>;
struct Binding { Declaration declaration; Uuid uuid; };
inline bool valid(const Uuid& id) noexcept {
    return (id[6] & 0xf0) == 0x40 && (id[8] & 0xc0) == 0x80;
}
inline Uuid decode(const std::vector<std::uint8_t>& wire) {
    if (wire.size() != 16) throw std::invalid_argument("symbol UUID length");
    Uuid id{};
    for (std::size_t i = 0; i < id.size(); ++i) id[i] = wire[i];
    if (!valid(id)) throw std::invalid_argument("symbol UUID version or variant");
    return id;
}
class Catalog {
public:
    static constexpr std::size_t max_entries = 4096;
    static constexpr std::size_t max_bytes = 1048576;
    explicit Catalog(const std::vector<Binding>& bindings) {
        if (bindings.size() > max_entries) throw std::length_error("symbol count");
        std::size_t bytes = 0;
        for (const auto& b : bindings) {
            const auto& ns = b.declaration.first;
            const auto& name = b.declaration.second;
            if (ns.empty() || ns.size() > 256 || name.empty() || name.size() > 256 || !valid(b.uuid))
                throw std::invalid_argument("invalid manifest binding");
            const auto cost = ns.size() + name.size() + 16;
            if (cost > max_bytes - bytes) throw std::length_error("symbol byte budget");
            bytes += cost;
            if (!forward_.emplace(b.declaration, b.uuid).second ||
                !reverse_.emplace(b.uuid, b.declaration).second)
                throw std::invalid_argument("duplicate manifest binding");
        }
    }
    // Trusted compiler-side lookup; caller checks declaration visibility.
    Uuid resolve(const Declaration& declaration) const { return forward_.at(declaration); }
    // Transport boundary: grants are supplied by the trusted admission layer.
    Uuid receive(const std::vector<std::uint8_t>& wire,
                 const std::set<Declaration>& grants) const {
        const auto id = decode(wire);
        const auto it = reverse_.find(id);
        if (it == reverse_.end() || grants.count(it->second) == 0)
            throw std::out_of_range("unknown or denied symbol UUID");
        return id;
    }
    std::size_t size() const noexcept { return forward_.size(); }
private:
    std::map<Declaration, Uuid> forward_;
    std::map<Uuid, Declaration> reverse_;
};
} // namespace oreslang::distributed_symbol_v1
