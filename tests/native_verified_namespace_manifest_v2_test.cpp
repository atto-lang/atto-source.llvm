#include "oreslang/frontend/VerifiedNamespaceManifestV2.hpp"
#include <openssl/sha.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
using namespace oreslang::frontend;
static_assert(!std::is_default_constructible<VerifiedNamespaceManifestV2>::value,
              "unverified code must not forge an admitted inventory");
static std::string sha(std::string_view bytes) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    static constexpr char hex[]="0123456789abcdef";
    std::string s;
    for (unsigned char c: digest) {s+=hex[c>>4];s+=hex[c&15];}
    return s;
}
template<class F> static void reject(F&& run) { try {run();} catch(const std::invalid_argument&) {return;} throw std::runtime_error("unexpected accept");}
int main() {
    const std::string manifest="schema_version = \"2\"\n[project]\nroot = \".\"\n[packages.checkout]\nfiles = [\"src/billing/charge.ores\",\n \"src/billing/refund.ores\"]\n";
    std::map<std::string,std::string> sources{{"src/billing/charge.ores","charge"},{"src/billing/refund.ores","refund"}};
    const std::string commitment = "src/billing/charge.ores\n"+sha("charge")+"\nsrc/billing/refund.ores\n"+sha("refund")+"\n";
    auto admit=[&](std::string_view m, std::map<std::string,std::string> s, std::string_view msha,std::string_view ssha){return verify_namespace_manifest_v2(m,msha,s,ssha,sha);};
    int checked=0;
    auto p=admit(manifest,sources,sha(manifest),sha(commitment));
    if(p.package_files().at("checkout").size()!=2 || p.sources_sha256() != sha(commitment))
        throw std::runtime_error("positive v2 inventory wrong");
    ++checked;
    reject([&]{admit(manifest,sources,sha("tampered"),sha(commitment));}); ++checked;
    reject([&]{auto s=sources;s.erase("src/billing/refund.ores");admit(manifest,s,sha(manifest),sha(commitment));}); ++checked;
    reject([&]{auto s=sources;s["src/billing/refund.ores"]="changed";admit(manifest,s,sha(manifest),sha(commitment));}); ++checked;
    reject([&]{auto s=sources;s["src/extra.ores"]="x";admit(manifest,s,sha(manifest),sha(commitment));}); ++checked;
    for (auto bad: {"src/../charge.ores","./src/charge.ores","src//charge.ores","/absolute.ores","src\\charge.ores"}) {
        std::string m="schema_version = \"2\"\n[project]\nroot = \".\"\n[packages.checkout]\nfiles = [\""+std::string(bad)+"\"]\n";
        reject([&]{admit(m,{{bad,"ok"}},sha(m),sha("irrelevant"));}); ++checked;
    }
    for (auto bad: {"schema_version = \"2\"\nschema_version = \"2\"\n[project]\nroot = \".\"\n[packages.checkout]\nfiles = [\"src/a.ores\"]\n",
                    "schema_version = \"2\"\n[project]\nroot = \".\"\n[namespace_dependencies.foo]\nmanifest = \"../foo/.ores.pkg.toml\"\n[packages.checkout]\nfiles = [\"src/a.ores\"]\n",
                    "schema_version = \"2\"\n[project]\nroot = \"../\"\n[packages.checkout]\nfiles = [\"src/a.ores\"]\n"}) {
        reject([&]{admit(bad,{{"src/a.ores","ok"}},sha(bad),sha("irrelevant"));}); ++checked;
    }
    // Two physical package declarations can coexist without assigning the
    // same member to different owners. The digest covers the entire set.
    const std::string multi = "schema_version = \"2\"\n[project]\nroot = \".\"\n"
        "[packages.checkout]\nfiles = [\"src/billing/charge.ores\"]\n"
        "[packages.other]\nfiles = [\"src/billing/refund.ores\"]\n";
    if (admit(multi, sources, sha(multi), sha(commitment)).package_files().size() != 2)
        throw std::runtime_error("valid multiple package inventories were rejected");
    ++checked;
    const std::string duplicated = "schema_version = \"2\"\n[project]\nroot = \".\"\n"
        "[packages.checkout]\nfiles = [\"src/a.ores\"]\n"
        "[packages.other]\nfiles = [\"src/a.ores\"]\n";
    reject([&]{admit(duplicated, {{"src/a.ores", "ok"}}, sha(duplicated), sha("irrelevant"));}); ++checked;
    reject([&]{admit(manifest, sources, std::string(64, 'A'), sha(commitment));}); ++checked;
    reject([&]{verify_namespace_manifest_v2(manifest, sha(manifest), sources,
             sha(commitment), NamespaceSha256{});}); ++checked;
    reject([&]{admit(manifest, sources, sha(manifest), sha("wrong source pin"));}); ++checked;
    const std::string duplicate_key = "schema_version = \"2\"\n[project]\nroot = \".\"\n"
        "[packages.checkout]\nfiles = [\"src/a.ores\"]\nfiles = [\"src/a.ores\"]\n";
    reject([&]{admit(duplicate_key, {{"src/a.ores", "ok"}},
                     sha(duplicate_key), sha("irrelevant"));}); ++checked;
    const std::string oversized = manifest + std::string(1024 * 1024, ' ');
    reject([&]{admit(oversized, sources, sha(oversized), sha(commitment));}); ++checked;
    std::cout << "PASS v2 manifest inventory cases=" << checked << "\n";
}
