#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oreslang::runtime {
class PersistentActorMap final {
public:
    using MapRef = std::shared_ptr<const PersistentActorMap>;
    class Value final {
    public:
        using Storage = std::variant<bool, std::int64_t, double, std::string, MapRef>;
        Value(bool v): value_(v) {}
        Value(std::int64_t v): value_(v) {}
        Value(int v): value_(static_cast<std::int64_t>(v)) {}
        Value(double v): value_(v) {
            if (!(v == v)) throw std::invalid_argument("NaN actor value");
        }
        Value(std::string v): value_(std::move(v)) {}
        Value(const char* v): value_(std::string(v)) {}
        explicit Value(MapRef v): value_(std::move(v)) {
            if (!std::get<MapRef>(value_))
                throw std::invalid_argument("null nested actor map");
        }
        const Storage& storage() const noexcept { return value_; }
        bool same_update_value(const Value& other) const {
            if (value_.index() != other.value_.index()) return false;
            if (std::holds_alternative<MapRef>(value_))
                return std::get<MapRef>(value_) == std::get<MapRef>(other.value_);
            return value_ == other.value_;
        }
    private:
        Storage value_;
    };

    static PersistentActorMap empty() { return PersistentActorMap(nullptr, 0); }
    std::size_t size() const noexcept { return count_; }
    const Value* get(const std::string& key) const {
        return find(root_, key, hash(key), 0);
    }
    bool contains(const std::string& key) const { return get(key) != nullptr; }

    PersistentActorMap with(std::string key, Value value) const {
        if (key.empty() || key.size() > 4096)
            throw std::invalid_argument("persistent actor key limit");
        bool added = false;
        auto next = put(root_, Leaf{std::move(key), std::move(value), 0}, 0, added);
        if (next == root_) return *this;
        return PersistentActorMap(std::move(next), count_ + (added ? 1u : 0u));
    }

    PersistentActorMap without(const std::string& key) const {
        if (key.empty()) throw std::invalid_argument("persistent actor key empty");
        bool removed = false;
        auto next = erase(root_, key, hash(key), 0, removed);
        if (!removed) return *this;
        return PersistentActorMap(std::move(next), count_ - 1);
    }

private:
    struct Node;
    using NodePtr = std::shared_ptr<const Node>;
    struct Leaf { std::string key; Value value; std::uint32_t h; };
    struct Collision { std::uint32_t h; std::vector<Leaf> leaves; };
    struct Branch { std::uint32_t bitmap; std::vector<NodePtr> children; };
    struct Node {
        std::variant<Leaf, Collision, Branch> payload;
        explicit Node(Leaf v): payload(std::move(v)) {}
        explicit Node(Collision v): payload(std::move(v)) {}
        explicit Node(Branch v): payload(std::move(v)) {}
    };

    static constexpr std::size_t kMaxCollision = 64;
    NodePtr root_;
    std::size_t count_;
    PersistentActorMap(NodePtr root, std::size_t count)
        : root_(std::move(root)), count_(count) {}

    static std::uint32_t hash(const std::string& key) {
        std::uint32_t h = 0;
        for (unsigned char c: key) h = h * 31u + c;
        return h ^ (h >> 16);
    }
    static std::uint32_t bit(std::uint32_t h, unsigned shift) {
        return 1u << ((h >> shift) & 31u);
    }
    static std::size_t offset(std::uint32_t bitmap, std::uint32_t b) {
#if defined(__GNUC__) || defined(__clang__)
        return static_cast<std::size_t>(__builtin_popcount(bitmap & (b - 1u)));
#else
        std::uint32_t x = bitmap & (b - 1u);
        std::size_t n = 0;
        while (x) { x &= x - 1u; ++n; }
        return n;
#endif
    }

    static NodePtr node(Leaf v) {
        return std::make_shared<const Node>(std::move(v));
    }
    static NodePtr node(Collision v) {
        return std::make_shared<const Node>(std::move(v));
    }
    static NodePtr node(Branch v) {
        return std::make_shared<const Node>(std::move(v));
    }
    static std::uint32_t common_hash(const NodePtr& n) {
        if (auto p = std::get_if<Leaf>(&n->payload)) return p->h;
        if (auto p = std::get_if<Collision>(&n->payload)) return p->h;
        throw std::logic_error("branch has no common hash");
    }

    static NodePtr merge(NodePtr left, NodePtr right, unsigned shift) {
        auto lh = common_hash(left), rh = common_hash(right);
        if (lh == rh) throw std::logic_error("equal hashes must collide before merge");
        if (shift > 30) throw std::logic_error("persistent hash trie depth exceeded");
        auto lb = bit(lh, shift), rb = bit(rh, shift);
        if (lb == rb)
            return node(Branch{lb, {merge(std::move(left), std::move(right), shift + 5)}});
        if (lb < rb)
            return node(Branch{lb | rb, {std::move(left), std::move(right)}});
        return node(Branch{lb | rb, {std::move(right), std::move(left)}});
    }

    static NodePtr put(NodePtr cur, Leaf entry, unsigned shift, bool& added) {
        entry.h = hash(entry.key);
        if (!cur) { added = true; return node(std::move(entry)); }
        if (auto leaf = std::get_if<Leaf>(&cur->payload)) {
            if (leaf->h == entry.h && leaf->key == entry.key) {
                if (leaf->value.same_update_value(entry.value)) return cur;
                return node(std::move(entry));
            }
            added = true;
            if (leaf->h == entry.h)
                return node(Collision{entry.h, {*leaf, std::move(entry)}});
            return merge(cur, node(std::move(entry)), shift);
        }
        if (auto collision = std::get_if<Collision>(&cur->payload)) {
            if (collision->h != entry.h) {
                added = true;
                return merge(cur, node(std::move(entry)), shift);
            }
            auto leaves = collision->leaves;
            for (auto& old : leaves) {
                if (old.key != entry.key) continue;
                if (old.value.same_update_value(entry.value)) return cur;
                old = std::move(entry);
                return node(Collision{collision->h, std::move(leaves)});
            }
            if (leaves.size() >= kMaxCollision)
                throw std::length_error("persistent collision bucket limit");
            leaves.push_back(std::move(entry));
            added = true;
            return node(Collision{collision->h, std::move(leaves)});
        }
        const auto& branch = std::get<Branch>(cur->payload);
        auto b = bit(entry.h, shift);
        auto idx = offset(branch.bitmap, b);
        auto children = branch.children;
        if (!(branch.bitmap & b)) {
            children.insert(children.begin() + static_cast<std::ptrdiff_t>(idx),
                            node(std::move(entry)));
            added = true;
            return node(Branch{branch.bitmap | b, std::move(children)});
        }
        auto replacement = put(children[idx], std::move(entry), shift + 5, added);
        if (replacement == children[idx]) return cur;
        children[idx] = std::move(replacement);
        return node(Branch{branch.bitmap, std::move(children)});
    }

    static NodePtr erase(NodePtr cur, const std::string& key,
                         std::uint32_t h, unsigned shift, bool& removed) {
        if (!cur) return nullptr;
        if (auto leaf = std::get_if<Leaf>(&cur->payload)) {
            if (leaf->h == h && leaf->key == key) {
                removed = true;
                return nullptr;
            }
            return cur;
        }
        if (auto collision = std::get_if<Collision>(&cur->payload)) {
            if (collision->h != h) return cur;
            auto leaves = collision->leaves;
            for (std::size_t i = 0; i < leaves.size(); ++i) {
                if (leaves[i].key != key) continue;
                removed = true;
                leaves.erase(leaves.begin() + static_cast<std::ptrdiff_t>(i));
                if (leaves.size() == 1) return node(std::move(leaves[0]));
                return node(Collision{h, std::move(leaves)});
            }
            return cur;
        }
        const auto& branch = std::get<Branch>(cur->payload);
        auto b = bit(h, shift);
        if (!(branch.bitmap & b)) return cur;
        auto idx = offset(branch.bitmap, b);
        auto replacement = erase(branch.children[idx], key, h, shift + 5, removed);
        if (!removed) return cur;
        auto children = branch.children;
        auto bitmap = branch.bitmap;
        if (replacement) children[idx] = std::move(replacement);
        else {
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(idx));
            bitmap &= ~b;
        }
        if (children.empty()) return nullptr;
        if (children.size() == 1 &&
            !std::holds_alternative<Branch>(children[0]->payload))
            return children[0];
        return node(Branch{bitmap, std::move(children)});
    }

    static const Value* find(NodePtr cur, const std::string& key,
                             std::uint32_t h, unsigned shift) {
        if (!cur) return nullptr;
        if (auto leaf = std::get_if<Leaf>(&cur->payload))
            return leaf->h == h && leaf->key == key ? &leaf->value : nullptr;
        if (auto collision = std::get_if<Collision>(&cur->payload)) {
            if (collision->h != h) return nullptr;
            for (const auto& leaf : collision->leaves)
                if (leaf.key == key) return &leaf.value;
            return nullptr;
        }
        const auto& branch = std::get<Branch>(cur->payload);
        auto b = bit(h, shift);
        if (!(branch.bitmap & b)) return nullptr;
        return find(branch.children[offset(branch.bitmap, b)], key, h, shift + 5);
    }
};

struct PureActorCommand final { std::uint64_t target; std::int64_t value; };
class PureActorTransition final {
public:
    PersistentActorMap state;
    std::vector<PureActorCommand> commands;
    PureActorTransition(PersistentActorMap next,
                        std::vector<PureActorCommand> effects)
        : state(std::move(next)), commands(std::move(effects)) {
        if (commands.size() > 64)
            throw std::length_error("pure actor command budget");
        for (const auto& command : commands)
            if (!command.target)
                throw std::invalid_argument("pure actor command target");
    }
};
using PureActorReducer =
    PureActorTransition (*)(const PersistentActorMap&, std::int64_t);
} // namespace oreslang::runtime
