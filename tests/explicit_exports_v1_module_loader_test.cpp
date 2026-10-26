#include "oreslang/frontend/ModuleLoader.hpp"
#include "oreslang/frontend/SourceInput.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create export fixture");
    out << text;
}

static oreslang::Program load(const fs::path& source) {
    SourceIdentity identity;
    const std::string text = read_regular_source(
        source.string().c_str(), 1024u * 1024u,
        "explicit export test source too large", &identity);
    require_valid_source_utf8(text);
    return oreslang::load_native_program(
        source.string(), text, identity, false, false, true);
}

template <typename F>
static void must_fail(std::string_view needle, F&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(needle) != std::string_view::npos)
            return;
        throw;
    }
    throw std::runtime_error("expected explicit export module-loader rejection");
}

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temp = fs::temp_directory_path() /
        ("ores-explicit-export-loader-" + std::to_string(stamp));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{temp};
    fs::create_directories(temp);

    {
        const fs::path dir = temp / "public";
        write_text(dir / "lib.ores", R"(
export module Lib;
define module Lib as
  export fnc answer;
  fnc answer(): int { return 42; }
end
)");
        write_text(dir / "main.ores",
                   "import module Lib from './lib'; "
                   "pub fnc main():int{return Lib.answer();}");
        (void)load(dir / "main.ores");
    }

    {
        const fs::path dir = temp / "alias";
        write_text(dir / "lib.ores", R"(
export module Lib as PublicLib;
define module Lib as
  export fnc answer;
  fnc answer(): int { return 42; }
end
)");
        write_text(dir / "main.ores",
                   "import module PublicLib from './lib'; "
                   "pub fnc main():int{return PublicLib.answer();}");
        (void)load(dir / "main.ores");
    }

    // A source alias is callable through its external name only. Its local
    // spelling must not become a public or native symbol by accident.
    {
        const fs::path dir = temp / "function-alias";
        write_text(dir / "lib.ores", R"(
export module Lib;
define module Lib as
  export fnc internal_answer as answer;
  fnc internal_answer(): int { return 42; }
end
)");
        write_text(dir / "main.ores",
                   "import module Lib from './lib'; "
                   "pub fnc main():int{return Lib.answer();}");
        (void)load(dir / "main.ores");

        write_text(dir / "main.ores",
                   "import module Lib from './lib'; "
                   "pub fnc main():int{return Lib.internal_answer();}");
        must_fail("imported function is private or unknown",
                  [&] { (void)load(dir / "main.ores"); });
    }

    // File/package source visibility is not cross-file PUBLIC import
    // authority until physical file and package identities are supported.
    for (const auto& scope : {"file", "pkg"}) {
        const fs::path dir = temp / (std::string("function-") + scope);
        write_text(dir / "lib.ores",
                   "export module Lib; define module Lib as "
                   "export[" + std::string(scope) + "] fnc internal_answer as answer; "
                   "fnc internal_answer():int{return 42;} end");
        write_text(dir / "main.ores",
                   "import module Lib from './lib'; "
                   "pub fnc main():int{return Lib.answer();}");
        must_fail("imported function is private or unknown",
                  [&] { (void)load(dir / "main.ores"); });
    }

    {
        const fs::path dir = temp / "private-module";
        write_text(dir / "lib.ores", R"(
define module Lib as
  export fnc answer;
  fnc answer(): int { return 42; }
end
)");
        write_text(dir / "main.ores",
                   "import module Lib from './lib'; "
                   "pub fnc main():int{return Lib.answer();}");
        must_fail("does not export selected module",
                  [&] { (void)load(dir / "main.ores"); });
    }

    {
        const fs::path dir = temp / "package-module";
        write_text(dir / "lib.ores", R"(
export[pkg] module Lib;
define module Lib as
  export fnc answer;
  fnc answer(): int { return 42; }
end
)");
        write_text(dir / "main.ores",
                   "import module Lib from './lib'; "
                   "pub fnc main():int{return Lib.answer();}");
        must_fail("does not export selected module",
                  [&] { (void)load(dir / "main.ores"); });
    }

    return 0;
}
