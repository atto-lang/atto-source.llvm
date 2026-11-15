#include "oreslang/frontend/Parser.hpp"
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
        {"take-after-use-reject.atto", "7757036e704fc334842cac8254863139d85d2935"},
        {"borrow-escape-reject.atto", "0c5d993b9c95d586ac2cb4018ce0df24856a36ae"},
        {"share-class-reject.atto", "3368f59e356003118e281fa73461e4185936e849"}
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
    std::string bytes(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    if (bytes.size() != static_cast<std::size_t>(count))
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

static void equivalent(std::string_view legacy, std::string_view spaced,
                       std::string_view compact, Expression::Kind kind) {
    for (const auto source : {legacy, spaced, compact}) {
        oreslang::Program holder;
        const Expression& expr = parsed(source, holder);
        if (expr.kind != kind || !expr.left)
            throw std::runtime_error("incorrect bang alias AST for " + std::string(source));
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

int main(int argc, char** argv) {
    if (argc != 2) throw std::runtime_error("expected canonical .atto fixture directory");
    const std::string root(argv[1]);
    equivalent("rt copy foo", "copy! foo", "copy!foo", Expression::Kind::Copy);
    equivalent("rt copy eqfoo", "copy! eqfoo", "copy!eqfoo", Expression::Kind::Copy);
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
    equivalent("rt share foo", "share! foo", "share!foo", Expression::Kind::Share);
    equivalent("rt mut borrow foo", "mut borrow! foo", "mut borrow!foo",
               Expression::Kind::BorrowMut);
    equivalent("rt borrow mut foo", "mut borrow! foo", "mut borrow!foo",
               Expression::Kind::BorrowMut);
    equivalent("rt copy(foo + 1)", "copy! (foo + 1)", "copy!(foo + 1)",
               Expression::Kind::Copy);
    equivalent("rt borrow foo.bar", "borrow! foo.bar", "borrow!foo.bar",
               Expression::Kind::Borrow);
    for (const auto invalid : {"copy !foo", "take !foo", "borrow !foo",
                               "mut borrow !foo", "share !foo", "copy!",
                               "mut borrow!"}) {
        reject(invalid);
    }
    // Production parser+ownership checker against exact checked-in .atto
    // fixtures. CTest must execute this; source-only fixture presence is not
    // itself an executable guest or native AOT claim.
    {
        const auto program = oreslang::parse_program(
            read_fixture(root, "borrow-call-accept.atto"));
        if (program.functions.size() != 3)
            throw std::runtime_error("native borrowed-argument admission missing");
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
    semantic_reject(read_fixture(root, "take-after-use-reject.atto"), "moved");
    semantic_reject(read_fixture(root, "borrow-escape-reject.atto"), "borrowed reference");
    semantic_reject(read_fixture(root, "share-class-reject.atto"), "non-Copy");

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

    // A genuine native parser+ownership check, not a lexical-model-only test.
    for (const auto source : {"copy!41", "copy! 41", "rt copy 41"}) {
        const auto program = oreslang::parse_program(
            std::string("pub fnc main(): int { return ") + source + "; }");
        if (program.functions.empty() || !program.functions[0].result)
            throw std::runtime_error("compiler ownership admission missing");
    }
    return 0;
}
