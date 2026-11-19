#include "oreslang/frontend/VerifiedNamespaceSourceSnapshotV2.hpp"
#include <openssl/sha.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
namespace fs = std::filesystem;
using namespace oreslang::frontend;
static_assert(!std::is_default_constructible<VerifiedNamespaceSourceSnapshotV2>::value,
              "unverified code must not forge a source snapshot");
static std::string sha(std::string_view bytes) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for(unsigned char byte: digest) {out += hex[byte>>4]; out += hex[byte&15];}
    return out;
}
static void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("fixture write failed");
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
template <class F> static void reject(F&& action) {
    try {action();} catch (const std::exception&) {return;}
    throw std::runtime_error("unverified source was accepted");
}
int main() {
    const fs::path work=fs::temp_directory_path()/("namespace-v2-snapshot-"+std::to_string(::getpid()));
    struct Cleanup { fs::path p; ~Cleanup() {std::error_code e; fs::remove_all(p,e);} } cleanup{work};
    const std::string manifest="schema_version = \"2\"\n[project]\nroot = \".\"\n"
        "[packages.checkout]\nfiles = [\"src/charge.ores\", \"src/refund.ores\"]\n";
    const std::string relative1="src/charge.ores", relative2="src/refund.ores";
    write(work/".ores.pkg.toml", manifest);
    write(work/relative1, "charge");
    write(work/relative2, "refund");
    const auto members=std::vector<std::string>{relative1,relative2};
    const auto digest=sha(relative1+"\n"+sha("charge")+"\n"+relative2+"\n"+sha("refund")+"\n");
    const auto admit=[&]{return load_verified_namespace_source_snapshot_v2(work,members,sha(manifest),digest,sha);};
    int n=0;
    const auto snapshot=admit();
    if (snapshot.source_bytes().at(relative1)!="charge" ||
        snapshot.inventory().package_files().at("checkout").size()!=2)
        throw std::runtime_error("verified snapshot mismatch");
    ++n;
    reject([&]{ (void)load_verified_namespace_source_snapshot_v2(work,members,sha("wrong"),digest,sha);}); ++n;
    reject([&]{ (void)load_verified_namespace_source_snapshot_v2(work,members,sha(manifest),sha("wrong"),sha);}); ++n;
    reject([&]{ (void)load_verified_namespace_source_snapshot_v2(work,{relative1},sha(manifest),digest,sha);}); ++n;
    reject([&]{ (void)load_verified_namespace_source_snapshot_v2(work,{relative1,relative2,relative2},sha(manifest),digest,sha);}); ++n;
    write(work/relative1,"evil");
    reject([&]{ (void)admit(); }); ++n;
    if(snapshot.source_bytes().at(relative1)!="charge")
        throw std::runtime_error("verified source snapshot changed with disk");
    ++n;
    write(work/relative1,"charge");
    fs::rename(work/relative2,work/"src/other.ores");
    fs::create_symlink("other.ores",work/relative2);
    reject([&]{ (void)admit();}); ++n;
    fs::remove(work/relative2);
    fs::rename(work/"src/other.ores",work/relative2);
    write(work/"src/.ores.pkg.toml","schema_version = \"2\"\n");
    reject([&]{ (void)admit();}); ++n;
    fs::remove(work/"src/.ores.pkg.toml");
    if(admit().source_bytes().size()!=2) throw std::runtime_error("restore mismatch");
    ++n;
    std::cout<<"PASS source snapshot v2 cases="<<n<<"\n";
}
