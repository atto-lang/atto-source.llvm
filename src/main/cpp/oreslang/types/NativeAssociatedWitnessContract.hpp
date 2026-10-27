#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::types {

// Static type projections only. The source parser and native guest lowering
// are separate work; no runtime dictionary creation or lookup is authorized.
struct AssociatedTypeTerm {
    std::string module;
    std::string name;
    std::vector<AssociatedTypeTerm> args;
    bool variable = false;

    static AssociatedTypeTerm named(std::string module, std::string name,
            std::vector<AssociatedTypeTerm> args = {}) {
        return {std::move(module), std::move(name), std::move(args), false};
    }
    static AssociatedTypeTerm var(std::string name) {
        return {"", std::move(name), {}, true};
    }
    bool operator==(const AssociatedTypeTerm& rhs) const {
        return module == rhs.module && name == rhs.name
                && args == rhs.args && variable == rhs.variable;
    }
};

struct AssociatedWitness {
    std::string protocol_module;
    std::string protocol_name;
    std::string defining_module;
    std::string dictionary_symbol;
    std::vector<AssociatedTypeTerm> head;
    std::map<std::string, AssociatedTypeTerm> equations;
};

struct AssociatedWitnessResolution {
    std::string dictionary_symbol;
    std::map<std::string, AssociatedTypeTerm> equations;
};

class NativeAssociatedWitnessRegistry final {
    struct Protocol {
        std::size_t arity = 0;
        std::set<std::string> associated;
    };
    using Bindings = std::map<std::string, AssociatedTypeTerm>;
    std::map<std::string, Protocol> protocols_;
    std::vector<AssociatedWitness> witnesses_;
    std::set<std::string> dictionary_symbols_;
    static constexpr std::size_t kMaxNodes = 1024;
    static constexpr std::size_t kMaxInstances = 4096;

    static std::string key(const std::string& module, const std::string& name) {
        if (module.empty() || name.empty()) throw std::invalid_argument("missing protocol identity");
        // Injective canonical identity: display concatenation (module::name)
        // aliases ("a::b", "c") and ("a", "b::c"). The length guards
        // prevent importing a foreign witness for the wrong protocol.
        return std::to_string(module.size()) + ":" + module + name;
    }
    static void validate_term(const AssociatedTypeTerm& term, std::size_t& nodes) {
        if (++nodes > kMaxNodes) throw std::invalid_argument("associated type graph budget exceeded");
        if (term.name.empty() || (term.variable && (!term.module.empty() || !term.args.empty()))
                || (!term.variable && term.module.empty()))
            throw std::invalid_argument("invalid static type term");
        for (const auto& child : term.args) validate_term(child, nodes);
    }
    static void collect(const AssociatedTypeTerm& t, std::set<std::string>& names) {
        if (t.variable) names.insert(t.name);
        else for (const auto& child : t.args) collect(child, names);
    }
    static void assert_bound(const AssociatedTypeTerm& t, const std::set<std::string>& names) {
        if (t.variable) {
            if (!names.count(t.name)) throw std::invalid_argument("unbound associated type variable");
        } else for (const auto& child : t.args) assert_bound(child, names);
    }
    static bool ground(const AssociatedTypeTerm& t) {
        if (t.variable) return false;
        for (const auto& child : t.args) if (!ground(child)) return false;
        return true;
    }
    static bool match(const AssociatedTypeTerm& pattern,
            const AssociatedTypeTerm& actual, Bindings& bindings) {
        if (pattern.variable) {
            auto it = bindings.find(pattern.name);
            if (it == bindings.end()) { bindings.emplace(pattern.name, actual); return true; }
            return it->second == actual;
        }
        if (pattern.module != actual.module || pattern.name != actual.name ||
                pattern.args.size() != actual.args.size()) return false;
        for (std::size_t i = 0; i < pattern.args.size(); ++i)
            if (!match(pattern.args[i], actual.args[i], bindings)) return false;
        return true;
    }
    static AssociatedTypeTerm substitute(const AssociatedTypeTerm& type,
            const Bindings& bindings) {
        if (type.variable) {
            auto it = bindings.find(type.name);
            if (it == bindings.end() || !ground(it->second))
                throw std::invalid_argument("associated type remained unbound");
            return it->second;
        }
        auto result = type;
        for (std::size_t i = 0; i < type.args.size(); ++i)
            result.args[i] = substitute(type.args[i], bindings);
        return result;
    }

    // Conservative first-order unification for overlap rejection.
    static AssociatedTypeTerm rename(AssociatedTypeTerm t, const std::string& side) {
        if (t.variable) t.name = side + t.name;
        else for (auto& child : t.args) child = rename(child, side);
        return t;
    }
    static AssociatedTypeTerm deref(AssociatedTypeTerm t, const Bindings& substitutions) {
        std::size_t steps = 0;
        while (t.variable) {
            auto it = substitutions.find(t.name);
            if (it == substitutions.end()) break;
            t = it->second;
            if (++steps > kMaxNodes)
                throw std::invalid_argument("cyclic overlap substitutions");
        }
        return t;
    }
    static bool occurs(const std::string& v, AssociatedTypeTerm t,
            const Bindings& subs) {
        t = deref(std::move(t), subs);
        if (t.variable) return v == t.name;
        for (const auto& child : t.args) if (occurs(v, child, subs)) return true;
        return false;
    }
    static bool unify(AssociatedTypeTerm left, AssociatedTypeTerm right,
            Bindings& substitutions) {
        left = deref(std::move(left), substitutions);
        right = deref(std::move(right), substitutions);
        if (left == right) return true;
        if (left.variable) {
            if (occurs(left.name, right, substitutions)) return false;
            substitutions.emplace(left.name, std::move(right));
            return true;
        }
        if (right.variable) {
            if (occurs(right.name, left, substitutions)) return false;
            substitutions.emplace(right.name, std::move(left));
            return true;
        }
        if (left.module != right.module || left.name != right.name ||
                left.args.size() != right.args.size()) return false;
        for (std::size_t i = 0; i < left.args.size(); ++i)
            if (!unify(left.args[i], right.args[i], substitutions)) return false;
        return true;
    }
    static bool overlaps(const std::vector<AssociatedTypeTerm>& a,
            const std::vector<AssociatedTypeTerm>& b) {
        Bindings subs;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!unify(rename(a[i], "L:"), rename(b[i], "R:"), subs)) return false;
        return true;
    }

public:
    void define(std::string module, std::string name, std::size_t arity,
            std::set<std::string> associated) {
        if (arity == 0 || arity > 64 || associated.empty() || associated.size() > 64)
            throw std::invalid_argument("invalid associated protocol declaration");
        for (const auto& member : associated) {
            if (member.empty() || member[0] < 'a' || member[0] > 'z' ||
                    !std::all_of(member.begin(), member.end(), [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                    })) throw std::invalid_argument("associated member must be snake_case");
        }
        const auto id = key(module, name);
        if (!protocols_.emplace(id, Protocol{arity, std::move(associated)}).second)
            throw std::invalid_argument("duplicate associated protocol");
    }

    void register_witness(AssociatedWitness witness) {
        auto it = protocols_.find(key(witness.protocol_module, witness.protocol_name));
        if (it == protocols_.end() || witness.head.size() != it->second.arity)
            throw std::invalid_argument("unknown protocol or incorrect witness arity");
        if (witness.equations.size() != it->second.associated.size() ||
                !std::all_of(witness.equations.begin(), witness.equations.end(),
                    [&](const auto& entry) { return it->second.associated.count(entry.first) != 0; }))
            throw std::invalid_argument("associated witness equations incomplete");
        if (witnesses_.size() >= kMaxInstances || witness.dictionary_symbol.empty() ||
                dictionary_symbols_.count(witness.dictionary_symbol))
            throw std::invalid_argument("dictionary symbol duplicate or instance budget exceeded");
        std::size_t nodes = 0;
        std::set<std::string> bound;
        bool owns_head = false;
        for (const auto& head : witness.head) {
            validate_term(head, nodes);
            collect(head, bound);
            if (!head.variable && head.module == witness.defining_module)
                owns_head = true;
        }
        if (witness.defining_module != witness.protocol_module && !owns_head)
            throw std::invalid_argument("orphan associated witness");
        for (const auto& equation : witness.equations) {
            validate_term(equation.second, nodes);
            assert_bound(equation.second, bound);
        }
        for (const auto& other : witnesses_) {
            if (other.protocol_module == witness.protocol_module &&
                    other.protocol_name == witness.protocol_name &&
                    overlaps(other.head, witness.head))
                throw std::invalid_argument("overlapping associated witnesses");
        }
        dictionary_symbols_.insert(witness.dictionary_symbol);
        witnesses_.push_back(std::move(witness));
    }

    AssociatedWitnessResolution resolve(const std::string& module,
            const std::string& protocol, const std::vector<AssociatedTypeTerm>& head) const {
        const auto it = protocols_.find(key(module, protocol));
        if (it == protocols_.end() || head.size() != it->second.arity)
            throw std::invalid_argument("missing associated protocol / argument arity");
        std::size_t nodes = 0;
        for (const auto& type : head) {
            validate_term(type, nodes);
            if (!ground(type)) throw std::invalid_argument("unresolved associated witness argument");
        }
        bool found = false;
        AssociatedWitnessResolution selected;
        for (const auto& witness : witnesses_) {
            if (witness.protocol_module != module || witness.protocol_name != protocol) continue;
            Bindings bindings;
            bool okay = true;
            for (std::size_t i = 0; i < head.size(); ++i)
                if (!match(witness.head[i], head[i], bindings)) { okay = false; break; }
            if (!okay) continue;
            if (found) throw std::logic_error("ambiguous associated witness");
            selected.dictionary_symbol = witness.dictionary_symbol;
            for (const auto& [member, type] : witness.equations)
                selected.equations.emplace(member, substitute(type, bindings));
            found = true;
        }
        if (!found) throw std::invalid_argument("missing static associated witness");
        return selected;
    }
};

} // namespace oreslang::types
