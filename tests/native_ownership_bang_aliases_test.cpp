#include "oreslang/frontend/Parser.hpp"
#include "oreslang/types/NativeScalarTypes.hpp"
#include <openssl/sha.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using oreslang::Expression;

// Git hashes are against the exact reviewed blobs, not just filenames or
// an untrusted test oracle copied alongside a modified fixture.
static std::string git_blob_sha(const std::string& bytes) {
    std::string preimage = "blob " + std::to_string(bytes.size());
    preimage.push_back('\0');
    preimage += bytes;
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(preimage.data()),
         preimage.size(), digest);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(40);
    for (unsigned char byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

static std::string read_fixture(const std::string& root, const std::string& filename) {
    static const std::map<std::string, std::string_view> reviewed_pins{
        {"borrow-call-accept.atto", "8cf245cbd24dc158f50bb010cbdcf09e1e826c10"},
        {"borrow-mut-canonical-native.atto", "398c533b3758a7ec35cc7f5cdb8bffb40f1c8baa"},
        {"borrow-mut-compatible-native.atto", "7c551be97b88204296f0c00000018dcd4798bd3a"},
        {"rt-borrow-mut-native.atto", "e7b2cb178c266262a94df18dd88c46ae9d32632b"},
        {"take-after-use-reject.atto", "7757036e704fc334842cac8254863139d85d2935"},
        {"borrow-escape-reject.atto", "0c5d993b9c95d586ac2cb4018ce0df24856a36ae"},
        {"share-class-reject.atto", "7dfd33777320170333551a6941ddff9828bfe349"},
        {"copy-compact-native.atto", "7cb835795345423f606caeed9b8fd4beb1c0a976"},
        {"copy-spaced-native.atto", "911e84e65f1783e6ca61c544e3f5953d0deccd1a"},
        {"copy-deep-scalars-native.atto", "5209998955bc1b4fa7bd3ea21c4a708abde0f25f"},
        {"copy-deep-string-reject.atto", "cf20161c12731e340b78bf94c90e1b77c21728ba"},
        {"take-native.atto", "c6983ffa37413732d359e87613a8ea79aa9dd591"},
        {"share-native.atto", "5a1ac6ab8a4c4cffba2e21eeff02f2dd75414c30"}
    };
    const auto pin = reviewed_pins.find(filename);
    if (pin == reviewed_pins.end())
        throw std::runtime_error("unreviewed native ownership fixture");
    // Preserve the path's source-tree provenance, not merely its bytes:
    // ifstream follows symlinks silently. Only checked-in regular files may
    // act as a regression oracle, and no traversal through a linked fixture
    // directory is permitted.
    namespace fs = std::filesystem;
    const fs::path directory(root);
    if (!fs::is_directory(fs::symlink_status(directory)))
        throw std::runtime_error("invalid native ownership fixture directory");
    for (fs::path part = directory; !part.empty(); part = part.parent_path()) {
        if (fs::is_symlink(fs::symlink_status(part)))
            throw std::runtime_error("symlinked native ownership fixture directory");
        if (part == part.root_path()) break;
    }
    const fs::path candidate = directory / filename;
    if (!fs::is_regular_file(fs::symlink_status(candidate)))
        throw std::runtime_error("symlinked/nonregular native ownership fixture: " + filename);
    std::ifstream file(candidate, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("missing native ownership fixture: " + filename);
    const auto count = file.tellg();
    if (count <= 0 || count > 8192)
        throw std::runtime_error("oversized/invalid native ownership fixture: " + filename);
    file.seekg(0, std::ios::beg);
    // istreambuf_iterator reads to EOF and can exceed the validated size if
    // the fixture grows between tellg() and read(). Allocate only the
    // validated byte count and refuse any extra byte or short read.
    std::string bytes(static_cast<std::size_t>(count), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (file.gcount() != count ||
        file.peek() != std::char_traits<char>::eof() || file.bad())
        throw std::runtime_error("native ownership fixture changed while reading: " + filename);
    if (git_blob_sha(bytes) != pin->second)
        throw std::runtime_error("native ownership fixture Git blob drift: " + filename);
    return bytes;
}

static const Expression& parsed(std::string_view source, oreslang::Program& holder) {
    holder = oreslang::parse_unresolved_program(
        std::string("pub fnc main(): int { return ") + std::string(source) + "; }");
    if (holder.functions.size() != 1 || !holder.functions[0].result)
        throw std::runtime_error("expected one parsed native result");
    return *holder.functions[0].result;
}

// Comparison intentionally ignores byte offsets: source aliases differ in
// spelling/length, but must carry the exact same resolved ownership operation
// and complete operand tree (including member/index and nested expression).
static bool same_operand_tree(const Expression* a, const Expression* b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind || a->integer != b->integer ||
        a->boolean != b->boolean || a->name != b->name ||
        a->sum_type != b->sum_type ||
        a->arguments.size() != b->arguments.size() ||
        a->argument_names != b->argument_names ||
        a->type_arguments_present != b->type_arguments_present ||
        a->type_arguments.size() != b->type_arguments.size())
        return false;
    if (!same_operand_tree(a->left.get(), b->left.get()) ||
        !same_operand_tree(a->right.get(), b->right.get()) ||
        !same_operand_tree(a->condition.get(), b->condition.get()) ||
        !same_operand_tree(a->condition_lhs.get(), b->condition_lhs.get()) ||
        !same_operand_tree(a->condition_rhs.get(), b->condition_rhs.get()) ||
        !same_operand_tree(a->when_true.get(), b->when_true.get()) ||
        !same_operand_tree(a->when_false.get(), b->when_false.get()))
        return false;
    for (std::size_t i = 0; i < a->arguments.size(); ++i) {
        if (!same_operand_tree(a->arguments[i].get(), b->arguments[i].get()))
            return false;
    }
    for (std::size_t i = 0; i < a->type_arguments.size(); ++i) {
        const auto& x = a->type_arguments[i];
        const auto& y = b->type_arguments[i];
        if (x.name != y.name || x.type != y.type || x.infer != y.infer)
            return false;
    }
    return true;
}

static void equivalent(std::string_view legacy, std::string_view spaced,
                       std::string_view compact, Expression::Kind kind) {
    oreslang::Program expected_holder;
    const Expression& expected = parsed(legacy, expected_holder);
    if (expected.kind != kind || !expected.left)
        throw std::runtime_error("incorrect legacy ownership baseline");
    for (const auto source : {spaced, compact}) {
        oreslang::Program holder;
        const Expression& expr = parsed(source, holder);
        if (!same_operand_tree(&expected, &expr))
            throw std::runtime_error("ownership alias changed operand AST: " +
                                     std::string(source));
    }
}

static void reject(std::string_view input) {
    try {
        oreslang::Program parsed_source;
        (void)parsed(input, parsed_source);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("unexpected ownership spelling acceptance: " + std::string(input));
}

static void reject_with_diagnostic(std::string_view source,
                                   std::string_view diagnostic) {
    try {
        oreslang::Program holder;
        (void)parsed(source, holder);
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(diagnostic) != std::string_view::npos)
            return;
        throw std::runtime_error("wrong ownership error for " + std::string(source) +
                                 ": " + error.what());
    }
    throw std::runtime_error("missing ownership rejection for " + std::string(source));
}

int main(int argc, char** argv) {
    if (argc != 2) throw std::runtime_error("expected canonical .atto fixture directory");
    const std::string root(argv[1]);
    // Real native parser + type/ownership admission. No object emission
    // or executable guest claim is made by this unit test.
    for (const auto* body : {
            "let a <- 42; return a;",
            "let int a <- 42; return a;",
            "let a = 0; set a <- 42; return a;",
            "let bool flag <- true; return flag ? 42 : 0;"}) {
        const auto program = oreslang::parse_program(
            std::string("pub fnc main(): int { ") + body + " }");
        if (program.functions.size() != 1 || !program.functions[0].result)
            throw std::runtime_error("native literal arrow lost main");
    }
    for (const auto* body : {
            "const a <- 42; return a;",
            "int a <- 42; return a;",
            "val a <- 42; return a;",
            "let a = 0; set! a <- 42; return a;",
            "let a = 0; a <- 42; return a;",
            "set missing <- 42; return 0;",
            "const a = 0; set a <- 42; return a;",
            "let a = 0; set a <- true; return a;",
            "let a < - 42; return a;",
            "let a • 42; return a;",
            "let b = 42; let a <- b; return a;",
            "let a <- 21 + 21; return a;"}) {
        bool rejected = false;
        try {
            (void)oreslang::parse_program(
                std::string("pub fnc main(): int { ") + body + " }");
        } catch (const std::runtime_error&) { rejected = true; }
        if (!rejected)
            throw std::runtime_error(std::string("unexpected arrow admission: ") + body);
    }

    // Compiler-created ASTs must independently validate absolute-value
    // operand types, not rely solely on parser or codegen checks.
    {
        auto program = oreslang::parse_unresolved_program(
            "pub fnc main(): int { return 42; }");
        auto& expr = *program.functions.at(0).result;
        expr.left = std::make_unique<Expression>();
        expr.left->kind = Expression::Kind::Integer;
        expr.left->integer = -42;
        expr.kind = Expression::Kind::Abs;
        oreslang::validate_native_scalar_types(program);
        expr.left->kind = Expression::Kind::Boolean;
        bool rejected = false;
        try { oreslang::validate_native_scalar_types(program); }
        catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()).find("abs requires int") != std::string_view::npos;
        }
        if (!rejected) throw std::runtime_error("unchecked Abs accepted boolean");
        expr.left.reset();
        rejected = false;
        try { oreslang::validate_native_scalar_types(program); }
        catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()).find("abs requires an operand") != std::string_view::npos;
        }
        if (!rejected) throw std::runtime_error("unchecked Abs accepted missing operand");
    }

    // Each executable contract.py/Clang fixture is also bound to an immutable
    // Git blob in this compiler-owned checker: ordinary CTest source reads
    // alone are not a provenance assertion.
    for (const char* name : {"borrow-call-accept.atto",
                             "borrow-mut-canonical-native.atto",
                             "borrow-mut-compatible-native.atto",
                             "rt-borrow-mut-native.atto",
                             "take-after-use-reject.atto",
                             "borrow-escape-reject.atto",
                             "share-class-reject.atto",
                             "copy-compact-native.atto",
                             "copy-spaced-native.atto",
                             "copy-deep-scalars-native.atto",
                             "copy-deep-string-reject.atto",
                             "take-native.atto", "share-native.atto"}) {
        (void)read_fixture(root, name);
    }
    equivalent("rt copy foo", "copy! foo", "copy!foo", Expression::Kind::Copy);
    equivalent("rt copy eqfoo", "copy! eqfoo", "copy!eqfoo", Expression::Kind::Copy);
    for (const auto source : {"copy!! flag", "copy!!flag", "copy!!/* operand */flag"}) {
        oreslang::Program holder;
        const auto& expr = parsed(source, holder);
        if (expr.kind != Expression::Kind::DeepCopy || !expr.left ||
                expr.left->kind != Expression::Kind::Variable || expr.left->name != "flag")
            throw std::runtime_error("adjacent bangs must form a single deep copy");
    }
    for (const auto source : {"copy! !true", "copy!/* separator */!true"}) {
        oreslang::Program holder;
        const auto& expr = parsed(source, holder);
        if (expr.kind != Expression::Kind::Copy || !expr.left ||
                expr.left->kind != Expression::Kind::LogicalNot)
            throw std::runtime_error("separated logical negation must remain a separate operation");
    }
    // !eq is an atomic inequality token at an identifier boundary, not
    // an ownership marker followed by the identifier 'eq'.
    {
        oreslang::Program holder;
        const Expression& expr = parsed("copy!eq foo", holder);
        if (expr.kind != Expression::Kind::NotEqual)
            throw std::runtime_error("atomic !eq inequality parsed as ownership");
    }
    equivalent("rt take foo", "take! foo", "take!foo", Expression::Kind::Take);
    equivalent("rt borrow foo", "borrow! foo", "borrow!foo", Expression::Kind::Borrow);
    // mutfoo is an identifier; only a standalone mut keyword grants an exclusive loan.
    equivalent("rt borrow mutfoo", "borrow! mutfoo", "borrow!mutfoo",
               Expression::Kind::Borrow);
    equivalent("rt share foo", "share! foo", "share!foo", Expression::Kind::Share);
    equivalent("rt mut borrow foo", "mut borrow! foo", "mut borrow!foo",
               Expression::Kind::BorrowMut);
    equivalent("rt mut borrow foo", "borrow! mut foo", "borrow!mut foo",
               Expression::Kind::BorrowMut);
    equivalent("rt borrow mut foo", "borrow! mut foo", "borrow!mut foo",
               Expression::Kind::BorrowMut);
    equivalent("rt mut borrow(foo)", "borrow! mut (foo)", "borrow!mut(foo)",
               Expression::Kind::BorrowMut);
    equivalent("rt borrow mut(foo)", "borrow! mut (foo)", "borrow!mut(foo)",
               Expression::Kind::BorrowMut);
    equivalent("rt mut borrow foo.bar", "borrow! mut foo.bar", "borrow!mut foo.bar",
               Expression::Kind::BorrowMut);
    equivalent("rt borrow mut foo.bar", "borrow! mut foo.bar", "borrow!mut foo.bar",
               Expression::Kind::BorrowMut);
    equivalent("rt copy(foo + 1)", "copy! (foo + 1)", "copy!(foo + 1)",
               Expression::Kind::Copy);
    equivalent("rt borrow foo.bar", "borrow! foo.bar", "borrow!foo.bar",
               Expression::Kind::Borrow);
    for (const auto invalid : {"copy !foo", "take !foo", "borrow !foo",
                               "mut borrow !foo", "share !foo", "copy!",
                               "mut borrow!", "borrow! mut", "borrow!mut",
                               "borrow! mut mut foo", "mut borrow! mut foo"}) {
        reject(invalid);
    }
    // A catch-all parser error is not adequate evidence for duplicate-mut
    // rejection: assert the dedicated category on each ownership spelling.
    for (std::string_view invalid : {"borrow! mut mut foo",
                                     "borrow!mut mut foo",
                                     "mut borrow! mut foo",
                                     "rt mut borrow mut foo",
                                     "rt borrow mut mut foo"}) {
        reject_with_diagnostic(invalid, "duplicate mutable borrow qualifier");
    }
    // Keep mutfoo an ordinary identifier: a separate mut keyword is required.
    equivalent("rt borrow mutfoo", "borrow! mutfoo", "borrow!mutfoo",
               Expression::Kind::Borrow);
    // Production parser+ownership checker against exact checked-in .atto
    // fixtures. CTest must execute this; source-only fixture presence is not
    // itself an executable guest or native AOT claim.
    {
        const auto program = oreslang::parse_program(
            read_fixture(root, "borrow-call-accept.atto"));
        if (program.functions.size() != 3)
            throw std::runtime_error("native borrowed-argument admission missing");
        const auto mutable_program = oreslang::parse_program(
            read_fixture(root, "rt-borrow-mut-native.atto"));
        if (mutable_program.functions.size() != 3)
            throw std::runtime_error("native rt borrow mut argument admission missing");
    }
    auto semantic_reject = [](std::string_view source, std::string_view expected) {
        try {
            (void)oreslang::parse_program(source);
        } catch (const std::exception& e) {
            if (std::string(e.what()).find(expected) != std::string::npos)
                return;
            throw std::runtime_error("wrong native ownership denial: " +
                                     std::string(e.what()));
        }
        throw std::runtime_error("native ownership violation accepted");
    };
    // Both syntaxes must reach the real ownership checker before native
    // Clang O0/O2 execution gates can claim a mutable loan contract.
    for (const char* fixture : {"borrow-mut-canonical-native.atto", "borrow-mut-compatible-native.atto"}) {
        const auto program = oreslang::parse_program(read_fixture(root, fixture));
        if (program.functions.size() != 3)
            throw std::runtime_error("mutable borrow guest admission missing");
    }
    semantic_reject(read_fixture(root, "take-after-use-reject.atto"), "moved");
    semantic_reject(read_fixture(root, "borrow-escape-reject.atto"), "borrowed reference");
    semantic_reject(read_fixture(root, "share-class-reject.atto"), "non-Copy");
    semantic_reject(read_fixture(root, "copy-deep-string-reject.atto"), "E_COPY_GRAPH_UNPROVEN");

    // The native unary grammar is limited to 64 nested operands; aliases
    // must not bypass that pre-existing stack/complexity budget.
    {
        std::string deeply_nested;
        for (int i = 0; i < 100; ++i) deeply_nested += "copy!";
        deeply_nested += "41";
        try {
            oreslang::Program parsed_source;
            (void)parsed(deeply_nested, parsed_source);
            throw std::runtime_error("native ownership nesting limit bypassed");
        } catch (const std::exception& e) {
            if (std::string(e.what()).find("nesting limit exceeded") == std::string::npos)
                throw;
        }
    }

    // Nested comments must hide ownership operators before tokenization,
    // with a fixed admission budget instead of unbounded comment depth.
    {
        oreslang::Program holder;
        const Expression& expr = parsed(
            "/* outer /* take! foo */ tail /* share! foo */ */ copy!41", holder);
        if (expr.kind != Expression::Kind::Copy)
            throw std::runtime_error("nested comment leaked an ownership token");
    }
    {
        std::string excessive;
        for (int i = 0; i < 129; ++i) excessive += "/*";
        excessive += "copy!41";
        for (int i = 0; i < 129; ++i) excessive += "*/";
        reject_with_diagnostic(excessive, "block comment nesting exceeds 128");
    }

    // A genuine native parser+ownership check, not a lexical-model-only test.
    for (const auto source : {"copy!41", "copy! 41", "rt copy 41", "copy!! 41",
                              "(copy!!true) ? 42 : 0"}) {
        const auto program = oreslang::parse_program(
            std::string("pub fnc main(): int { return ") + source + "; }");
        if (program.functions.empty() || !program.functions[0].result)
            throw std::runtime_error("compiler ownership admission missing");
    }
    return 0;
}
