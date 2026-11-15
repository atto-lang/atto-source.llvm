#include "oreslang/frontend/NamespacePhysicalSourceV2.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
namespace fs = std::filesystem;
using namespace oreslang::frontend;
static void write(const fs::path& p, const std::string& value) {
    fs::create_directories(p.parent_path());
    std::ofstream stream(p, std::ios::binary);
    if (!stream) throw std::runtime_error("fixture write error");
    stream << value;
}
template<class F> static void rejected(F&& action) {
    try { action(); } catch(const std::exception&) { return; }
    throw std::runtime_error("expected fail closed");
}
int main() {
    const auto tmp=fs::temp_directory_path()/("namespace-v2-fd-test-"+std::to_string(::getpid()));
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p,ec); } } cleanup{tmp};
    const fs::path root=tmp/"real";
    const fs::path manifest=root/".ores.pkg.toml";
    const fs::path src=root/"src/pkg/entry.ores";
    const std::string contents="namespace pkg;\n";
    write(manifest,"schema_version = \"2\"\n");
    write(src,contents);
    const auto valid=[&]{ return read_namespace_v2_physical_files(root,{"src/pkg/entry.ores"}); };
    int n=0;
    auto snap=valid();
    if(snap.source_bytes.at("src/pkg/entry.ores")!=contents || snap.manifest_bytes.empty())
        throw std::runtime_error("valid snapshot wrong");
    ++n;
    rejected([&]{(void)read_namespace_v2_physical_files(root,{});}); ++n;
    rejected([&]{(void)read_namespace_v2_physical_files(root,{"src/pkg/entry.ores","src/pkg/entry.ores"});}); ++n;
    for(const auto& bad: {"src/../entry.ores","./src/pkg/entry.ores","src//pkg/entry.ores","/src/pkg/entry.ores","src\\pkg/entry.ores","src/pkg/entry.txt"}) {
        rejected([&]{(void)read_namespace_v2_physical_files(root,{bad});}); ++n;
    }
    rejected([&]{(void)read_namespace_v2_physical_files(root/"../real",{"src/pkg/entry.ores"});}); ++n;
    rejected([&]{(void)read_namespace_v2_physical_files(root,{"src/pkg/absent.ores"});}); ++n;
    write(root/"src/.ores.pkg.toml","schema_version = \"2\"\n");
    rejected([&]{(void)valid();}); ++n;
    fs::remove(root/"src/.ores.pkg.toml");
    fs::rename(src,root/"src/pkg/real.ores");
    fs::create_symlink("real.ores",src);
    rejected([&]{(void)valid();}); ++n;
    fs::remove(src);
    fs::rename(root/"src/pkg/real.ores",src);
    fs::rename(manifest,root/"real.toml");
    fs::create_symlink("real.toml",manifest);
    rejected([&]{(void)valid();}); ++n;
    fs::remove(manifest);
    fs::rename(root/"real.toml",manifest);
    fs::rename(root/"src",root/"real-src");
    fs::create_symlink("real-src",root/"src");
    rejected([&]{(void)valid();}); ++n;
    fs::remove(root/"src");
    fs::rename(root/"real-src",root/"src");
    fs::create_symlink("real",tmp/"root-alias");
    rejected([&]{(void)read_namespace_v2_physical_files(tmp/"root-alias",{"src/pkg/entry.ores"});}); ++n;
    fs::remove(tmp/"root-alias");
    rejected([&]{(void)read_namespace_v2_physical_files(root,{"src/pkg/entry.ores","src/pkg/extra.ores"});}); ++n;
    if (valid().source_bytes.size() != 1) throw std::runtime_error("restored snapshot wrong");
    ++n;
    // Controlled source-path identity race: an already opened descriptor is
    // unchanged but its directory entry has been replaced with identical
    // source bytes. The identity bridge must detect the different inode.
    {
        using namespace namespace_v2_physical_detail;
        Fd parent(::open((root/"src/pkg").c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.fd < 0) throw std::runtime_error("test cannot open provider directory");
        Fd original(::openat(parent.fd, "entry.ores", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (original.fd < 0) throw std::runtime_error("test cannot open source file");
        struct stat before{};
        if (::fstat(original.fd, &before) != 0) throw std::runtime_error("test cannot inspect inode");
        require_same_path_binding(parent.fd, "entry.ores", before);
        ++n;
        const auto replacement = root/"src/pkg/replacement.ores";
        write(replacement, contents);
        fs::rename(replacement, src);
        rejected([&]{ require_same_path_binding(parent.fd, "entry.ores", before); });
        ++n;
        if (valid().source_bytes.at("src/pkg/entry.ores") != contents)
            throw std::runtime_error("same-byte replacement not readable");
        ++n;
    }
    std::cout<<"PASS physical source v2 cases="<<n<<"\n";
}
