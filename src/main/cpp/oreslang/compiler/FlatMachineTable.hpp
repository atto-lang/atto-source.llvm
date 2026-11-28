#pragma once
#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace atto::machine {
// ASCII inspection profile only. Local slots never identify nominal messages
// or authorize delivery, state writes, persistence, or native code generation.
struct Edge { std::string event; std::size_t target; };
struct Node { std::string name; bool terminal; std::vector<Edge> edges; };
struct Transition { std::size_t slot, target; };
class Table {
    static constexpr std::size_t limit = 256;
    std::size_t initial_;
    std::vector<std::string> names_, events_;
    std::vector<std::vector<Transition>> rows_;
    explicit Table(std::size_t initial) : initial_(initial) {}
    static void require(bool ok) {
        if (!ok) throw std::invalid_argument("E_MACHINE_TABLE");
    }
    static bool name(const std::string& s) {
        if (s.empty() || s.size() > 1024) return false;
        auto letter = [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
        if (!letter(static_cast<unsigned char>(s.front()))) return false;
        return std::all_of(s.begin(), s.end(), [&](unsigned char c) { return letter(c) || (c >= '0' && c <= '9'); });
    }
public:
    static Table verify(std::size_t initial, const std::vector<Node>& nodes) {
        require(!nodes.empty() && nodes.size() <= limit && initial < nodes.size());
        Table out(initial);
        std::set<std::string> events;
        std::size_t count = 0;
        for (const auto& node : nodes) {
            require(name(node.name) && (out.names_.empty() || out.names_.back() < node.name));
            out.names_.push_back(node.name);
            require(!node.terminal || node.edges.empty());
            std::string previous;
            for (const auto& edge : node.edges) {
                require(++count <= limit && name(edge.event) && previous < edge.event && edge.target < nodes.size());
                events.insert(edge.event);
                previous = edge.event;
            }
        }
        std::vector<bool> seen(nodes.size());
        std::vector<std::size_t> pending{initial};
        std::size_t visited = 0;
        while (!pending.empty()) {
            const auto current = pending.back(); pending.pop_back();
            if (seen[current]) continue;
            seen[current] = true; ++visited;
            for (const auto& edge : nodes[current].edges) pending.push_back(edge.target);
        }
        require(visited == nodes.size());
        out.events_.assign(events.begin(), events.end());
        for (const auto& node : nodes) {
            std::vector<Transition> row;
            for (const auto& edge : node.edges) {
                const auto it = std::lower_bound(out.events_.begin(), out.events_.end(), edge.event);
                row.push_back({static_cast<std::size_t>(it - out.events_.begin()), edge.target});
            }
            out.rows_.push_back(std::move(row));
        }
        return out;
    }
    std::size_t initial() const { return initial_; }
    const std::vector<std::string>& names() const { return names_; }
    const std::vector<std::string>& events() const { return events_; }
    std::optional<std::size_t> successor(std::size_t node, std::size_t slot) const {
        require(node < rows_.size() && slot < events_.size());
        for (const auto& edge : rows_[node]) if (edge.slot == slot) return edge.target;
        return std::nullopt;
    }
};
} // namespace atto::machine
