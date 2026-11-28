// L1 borrow-conformance adapter: feed the canonical ownership-graph corpus of
// atto-lang/atto-borrow-checker.rs straight into this backend's in-pass
// ownership engine (OwnershipCfg.hpp) and report one
// `atto-borrow-graph-results-v1` line per case.
//
//   borrow_conformance_l1_adapter <graph-cases.json>            # JSON Lines to stdout
//   borrow_conformance_l1_adapter <graph-cases.json> --check <known-differences.tsv>
//
// --check compares each verdict with the corpus expectation (exact canonical
// code, as `atto-borrow-conform graph-results` does) and fails unless every
// difference is listed, with a reason, in the known-differences file. An
// `unsupported` case is a coverage gap, never a pass.
//
// Mapping (canonical op -> OwnershipCfg):
//   owner N               -> place "oN", uninitialized at entry
//   Init / Move into      -> Initialize
//   Read / Drop / Suspend -> Read / Drop / Suspend
//   Move{from,into}       -> Move(from), Initialize(into)
//   Borrow / EndBorrow    -> BorrowShared|BorrowExclusive / ReleaseShared|ReleaseExclusive,
//                            with the canonical LoanId as explicit provenance
//   Return                -> terminal block (OwnershipCfg checks exit loans itself)
// Any op without an exact counterpart (Write, WriteThrough, Copy, DeepCopy,
// Freeze, pins, weaks, tasks, actors, UnknownEffect) makes the case
// `unsupported`; so do graph shapes OwnershipCfg cannot express (a terminal
// block without Return, Return with successors, duplicate block ids, an
// EndBorrow of a loan that is never borrowed).
//
// Host-only evidence: this exercises the native engine on abstract graphs,
// not compiled .atto guests or the compiler's own lowering.

#include "oreslang/types/OwnershipCfg.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---- minimal strict JSON reader (objects, arrays, strings, ints, bools, null)

struct Json {
    enum class Type { Null, Bool, Int, Str, Arr, Obj } type = Type::Null;
    bool b = false;
    std::uint64_t n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const std::string& key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class Reader {
public:
    explicit Reader(const std::string& text) : t_(text) {}
    Json parse() {
        Json v = value(0);
        ws();
        if (i_ != t_.size()) fail("trailing characters");
        return v;
    }

private:
    const std::string& t_;
    std::size_t i_ = 0;

    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(std::string("corpus JSON: ") + what + " at byte " + std::to_string(i_));
    }
    void ws() {
        while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) ++i_;
    }
    bool eat(const char* lit) {
        const std::string l(lit);
        if (t_.compare(i_, l.size(), l) == 0) { i_ += l.size(); return true; }
        return false;
    }
    Json value(int depth) {
        if (depth > 64) fail("nesting too deep");
        ws();
        if (i_ >= t_.size()) fail("unexpected end");
        Json v;
        const char c = t_[i_];
        if (c == '{') {
            v.type = Json::Type::Obj;
            ++i_;
            ws();
            if (eat("}")) return v;
            for (;;) {
                ws();
                if (i_ >= t_.size() || t_[i_] != '"') fail("expected key");
                std::string key = str();
                ws();
                if (!eat(":")) fail("expected ':'");
                v.obj.emplace_back(std::move(key), value(depth + 1));
                ws();
                if (eat(",")) continue;
                if (eat("}")) return v;
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Json::Type::Arr;
            ++i_;
            ws();
            if (eat("]")) return v;
            for (;;) {
                v.arr.push_back(value(depth + 1));
                ws();
                if (eat(",")) continue;
                if (eat("]")) return v;
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = Json::Type::Str; v.s = str(); return v; }
        if (eat("true")) { v.type = Json::Type::Bool; v.b = true; return v; }
        if (eat("false")) { v.type = Json::Type::Bool; return v; }
        if (eat("null")) return v;
        if (std::isdigit(static_cast<unsigned char>(c))) {
            v.type = Json::Type::Int;
            while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_]))) {
                const std::uint64_t d = static_cast<std::uint64_t>(t_[i_] - '0');
                if (v.n > (UINT64_MAX - d) / 10) fail("integer overflow");
                v.n = v.n * 10 + d;
                ++i_;
            }
            return v;
        }
        fail("unexpected character");
    }
    std::string str() {
        ++i_;
        std::string out;
        while (i_ < t_.size() && t_[i_] != '"') {
            if (t_[i_] == '\\') {
                ++i_;
                if (i_ >= t_.size()) fail("bad escape");
                const char e = t_[i_];
                if (e == 'n') out += '\n';
                else if (e == 't') out += '\t';
                else if (e == 'u') { out += '?'; i_ += 4; }  // not used by the corpus
                else out += e;
                ++i_;
            } else {
                out += t_[i_++];
            }
        }
        if (i_ >= t_.size()) fail("unterminated string");
        ++i_;
        return out;
    }
};

const Json& need(const Json& o, const char* key) {
    const Json* v = o.get(key);
    if (!v) throw std::runtime_error(std::string("corpus: missing key ") + key);
    return *v;
}

std::uint64_t num(const Json& o, const char* key) { return need(o, key).n; }

// ---- lowering ----------------------------------------------------------

using namespace oreslang::ownership;
using Kind = Operation::Kind;

struct Unsupported { std::string reason; };

struct Lowered {
    Graph graph;
    std::set<std::uint64_t> initialized_by_move;  // places introduced as Move/Copy targets
};

std::string place(std::uint64_t owner) { return "o" + std::to_string(owner); }

Lowered lower(const Json& g) {
    Lowered out;
    const std::uint64_t entry = num(g, "entry");
    const auto& blocks = need(g, "blocks").arr;
    // OwnershipCfg indexes blocks 0..n-1 with entry 0.
    std::map<std::uint64_t, std::size_t> index;
    std::vector<const Json*> ordered;
    for (const auto& b : blocks)
        if (num(b, "id") == entry) ordered.push_back(&b);
    if (ordered.size() != 1) throw Unsupported{"entry block missing or duplicated"};
    for (const auto& b : blocks)
        if (num(b, "id") != entry) ordered.push_back(&b);
    for (std::size_t i = 0; i < ordered.size(); ++i)
        if (!index.emplace(num(*ordered[i], "id"), i).second)
            throw Unsupported{"duplicate block id"};

    std::set<std::uint64_t> owners;
    std::map<std::uint64_t, std::pair<std::uint64_t, bool>> loans;  // loan -> (owner, mutable)
    for (const auto* b : ordered)
        for (const auto& op : need(*b, "ops").arr) {
            const std::string& name = need(op, "op").s;
            for (const char* key : {"owner", "from", "into"})
                if (const Json* v = op.get(key)) owners.insert(v->n);
            if (name == "Borrow")
                loans[num(op, "loan")] = {num(op, "owner"), need(op, "kind").s == "Mutable"};
        }
    for (std::uint64_t o : owners) out.graph.entry[place(o)] = PlaceState(kUninitialized);

    for (const auto* b : ordered) {
        BasicBlock block;
        const auto& ops = need(*b, "ops").arr;
        const auto& succ = need(*b, "successors").arr;
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const Json& op = ops[i];
            const std::string& name = need(op, "op").s;
            if (name == "Return") {
                if (i + 1 != ops.size() || !succ.empty())
                    throw Unsupported{"Return with successors or later operations"};
                continue;
            }
            if (name == "Init") {
                block.operations.emplace_back(Kind::Initialize, place(num(op, "owner")));
            } else if (name == "Read") {
                block.operations.emplace_back(Kind::Read, place(num(op, "owner")));
            } else if (name == "Drop") {
                block.operations.emplace_back(Kind::Drop, place(num(op, "owner")));
            } else if (name == "Suspend") {
                block.operations.emplace_back(Kind::Suspend, "");
            } else if (name == "Move") {
                block.operations.emplace_back(Kind::Move, place(num(op, "from")));
                block.operations.emplace_back(Kind::Initialize, place(num(op, "into")));
                out.initialized_by_move.insert(num(op, "into"));
            } else if (name == "Borrow") {
                const bool mut = need(op, "kind").s == "Mutable";
                block.operations.emplace_back(mut ? Kind::BorrowExclusive : Kind::BorrowShared,
                                              place(num(op, "owner")), num(op, "loan"));
            } else if (name == "EndBorrow") {
                const auto found = loans.find(num(op, "loan"));
                if (found == loans.end())
                    throw Unsupported{"EndBorrow of a loan never borrowed: no place to release"};
                block.operations.emplace_back(
                    found->second.second ? Kind::ReleaseExclusive : Kind::ReleaseShared,
                    place(found->second.first), found->first);
            } else {
                throw Unsupported{"no OwnershipCfg counterpart for " + name};
            }
        }
        if (succ.empty() && (ops.empty() || need(ops.back(), "op").s != "Return"))
            throw Unsupported{"terminal block without Return (OwnershipCfg has no exit marker)"};
        for (const auto& s : succ) {
            const auto found = index.find(s.n);
            // An unknown successor stays outside the graph: the engine rejects it.
            block.successors.push_back(found == index.end() ? ordered.size() : found->second);
        }
        out.graph.blocks.push_back(std::move(block));
    }
    return out;
}

// OwnershipCfg reports `ownership violation: <why> '<place>'`.
std::string canonical_code(const std::string& what, const Lowered& lowered) {
    auto has = [&](const char* s) { return what.find(s) != std::string::npos; };
    if (has("not definitely initialized")) return "E_USE_AFTER_MOVE_OR_DROP";
    if (has("initializing a live or ambiguous owner")) {
        const auto quote = what.find("'o");
        if (quote != std::string::npos) {
            const std::uint64_t owner = std::stoull(what.substr(quote + 2));
            if (lowered.initialized_by_move.count(owner)) return "E_OWNER_ID_REUSED_OR_BUDGET";
        }
        return "E_OWNER_DEF_REUSED";
    }
    if (has("read overlaps mutable loan") || has("moving/dropping a borrowed owner") ||
            has("shared borrow overlaps mutable loan") || has("overlapping exclusive loans") ||
            has("shared loan identity already live"))
        return "E_BORROW_CONFLICT";
    if (has("duplicate lexical loan identity")) return "E_LOAN_DEF_REUSED";
    if (has("unmatched") || has("foreign provenance") || has("unambiguous provenance"))
        return "E_UNKNOWN_LOAN";
    if (has("borrow crosses suspension")) return "E_SUSPEND_WITH_LIVE_LOAN";
    if (has("loan escapes CFG exit")) return "E_LOAN_ESCAPE";
    if (has("different live loan provenance at CFG join")) return "E_CFG_JOIN_UNPROVEN";
    if (has("unreachable CFG block")) return "E_CFG_UNREACHABLE";
    if (has("CFG edge outside graph")) return "E_CFG_EDGE";
    if (has("budget")) return "E_CFG_BUDGET";
    return "E_LLVM_UNMAPPED";
}

std::string esc(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out + "\"";
}

struct Result {
    std::string id, verdict, code, reason;
};

Result run_case(const Json& c) {
    Result r{need(c, "id").s, "", "", ""};
    try {
        const Lowered lowered = lower(need(c, "graph"));
        try {
            verify(lowered.graph);
            r.verdict = "accept";
        } catch (const std::runtime_error& e) {
            r.verdict = "reject";
            r.code = canonical_code(e.what(), lowered);
            r.reason = e.what();
        }
    } catch (const Unsupported& u) {
        r.verdict = "unsupported";
        r.reason = u.reason;
    }
    return r;
}

std::string line(const Result& r) {
    std::string out = R"({"schema":"atto-borrow-graph-results-v1","backend":"llvm","case":)" + esc(r.id) +
                      R"(,"verdict":)" + esc(r.verdict) + R"(,"code":)" + (r.code.empty() ? "null" : esc(r.code));
    if (!r.reason.empty()) out += std::string(r.verdict == "unsupported" ? R"(,"reason":)" : R"(,"message":)") + esc(r.reason);
    return out + "}";
}

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("cannot read ") + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && !(argc == 4 && std::string(argv[2]) == "--check")) {
        std::cerr << "usage: " << argv[0] << " <graph-cases.json> [--check <known-differences.tsv>]\n";
        return 2;
    }
    try {
        const std::string text = read_file(argv[1]);
        const Json corpus = Reader(text).parse();
        std::map<std::string, std::string> known;  // case -> "verdict code"
        if (argc == 4) {
            std::istringstream tsv(read_file(argv[3]));
            std::string row;
            while (std::getline(tsv, row)) {
                if (row.empty() || row[0] == '#') continue;
                std::istringstream cols(row);
                std::string id, verdict, code;
                std::getline(cols, id, '\t');
                std::getline(cols, verdict, '\t');
                std::getline(cols, code, '\t');
                known[id] = verdict + " " + (code == "-" ? "" : code);
            }
        }
        int unexplained = 0, conforms = 0, differs = 0, unsupported = 0;
        std::set<std::string> seen;
        for (const auto& c : need(corpus, "cases").arr) {
            const Result r = run_case(c);
            seen.insert(r.id);
            if (argc == 2) { std::cout << line(r) << "\n"; continue; }
            const bool expect_accept = need(c, "expected").s == "accept";
            const Json& diag = need(c, "diagnostic");
            const std::string expected_code = diag.type == Json::Type::Str ? diag.s : "";
            const bool ok = (r.verdict == "accept" && expect_accept) ||
                            (r.verdict == "reject" && !expect_accept && r.code == expected_code);
            if (ok) { ++conforms; continue; }
            // A coverage gap, counted in the summary; never a pass, never listed.
            if (r.verdict == "unsupported") { ++unsupported; continue; }
            ++differs;
            const std::string actual = r.verdict + " " + r.code;
            const auto k = known.find(r.id);
            if (k == known.end() || k->second != actual) {
                ++unexplained;
                std::cout << "UNEXPLAINED " << r.id << ": expected "
                          << (expect_accept ? "accept" : "reject " + expected_code)
                          << ", engine " << actual << (r.reason.empty() ? "" : " (" + r.reason + ")") << "\n";
            }
        }
        if (argc == 4) {
            for (const auto& k : known)
                if (!seen.count(k.first)) {
                    ++unexplained;
                    std::cout << "STALE known difference for missing case " << k.first << "\n";
                }
            std::cout << "summary conforms=" << conforms << " differs=" << differs
                      << " unsupported=" << unsupported << " unexplained=" << unexplained << "\n";
            return unexplained == 0 ? 0 : 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
