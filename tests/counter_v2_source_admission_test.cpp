#include "oreslang/frontend/CounterV2Source.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace counter = oreslang::frontend::counter_v2;

static std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read Counter source fixture");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static void write(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write Counter negative fixture");
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) throw std::runtime_error("cannot write complete Counter fixture");
}

struct Scratch {
    fs::path path;
    Scratch() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 128; ++i) {
            auto candidate = fs::temp_directory_path() /
                    ("ores-counter-v2-" + std::to_string(now) + "-" + std::to_string(i));
            if (fs::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create unique Counter fixture directory");
    }
    ~Scratch() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

static void mutate(const fs::path& root, std::string_view unit,
                   const std::string& before, const std::string& after) {
    const auto target = root / std::string(unit);
    const auto original = read(target);
    const auto at = original.find(before);
    if (at == std::string::npos || original.find(before, at + before.size()) != std::string::npos)
        throw std::runtime_error("invalid negative test source control");
    auto changed = original;
    changed.replace(at, before.size(), after);
    write(target, changed);
    bool denied = false;
    try { (void)counter::verify(root); }
    catch (const std::runtime_error&) { denied = true; }
    write(target, original);
    if (!denied) throw std::runtime_error("Counter v2 admitted unsupported changed source");
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected canonical Counter package root");
        const fs::path canonical = argv[1];
        const auto plan = counter::verify(canonical);
        if (plan.name_space != "counter" || plan.actor != "Counter" ||
            plan.state_field != "total" || plan.initial != 0 ||
            plan.stop != 4 || plan.messages != std::array<std::int64_t, 4>{1, 2, 3, 4})
            throw std::runtime_error("canonical Counter source plan mismatched");

        Scratch scratch;
        constexpr std::array<std::string_view, 7> units{
            ".atto.pkg.toml", "src/counter/actor.atto",
            "src/counter/contract.atto", "src/counter/main.atto",
            "src/counter/method.atto", "src/counter/receive.atto",
            "src/counter/state.atto"};
        for (auto unit : units) {
            auto path = scratch.path / std::string(unit);
            fs::create_directories(path.parent_path());
            fs::copy_file(canonical / std::string(unit), path);
        }

        mutate(scratch.path, "src/counter/receive.atto",
               "self.send(snapshot);", "self.send(s.total);");
        mutate(scratch.path, "src/counter/receive.atto",
               "val int snapshot", "let int snapshot");
        mutate(scratch.path, "src/counter/receive.atto",
               "critical-read", "critical -read");
        mutate(scratch.path, "src/counter/receive.atto",
               "-> s.total);", "-> s);");
        mutate(scratch.path, "src/counter/receive.atto",
               "if mail.value == 4;", "if mail.value = 4;");
        mutate(scratch.path, "src/counter/main.atto",
               "worker.send(2);", "worker.send(4);");
        mutate(scratch.path, "src/counter/main.atto",
               "worker.send(2);", "worker.send(9223372036854775807);");
        mutate(scratch.path, "src/counter/state.atto",
               "let int total : 0", "val int total : 0");
        mutate(scratch.path, "src/counter/actor.atto",
               "implements CounterContract", "implements UnknownContract");
        mutate(scratch.path, ".atto.pkg.toml",
               "name = \"counter-v2\"", "name = \"counter-forged\"");
        mutate(scratch.path, "src/counter/main.atto",
               "await worker.get_done();", "worker.get_done();");

        // A valid copied scalar can move into the message only after
        // its reader loan exits. No state view or host-side output replay
        // may be emitted from this source-admission-only frontend.
        std::cout << "LLVM_COUNTER_V2_STRUCTURAL_ADMISSION_PASS"
                  << " units=7 negative_mutations=11 guest_executed=false\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Counter v2 structural source admission failed: "
                  << ex.what() << '\n';
        return 1;
    }
}
