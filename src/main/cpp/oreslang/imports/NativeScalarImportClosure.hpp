#pragma once
// Opt-in, host-only scalar import closure. NOT namespace/module parity.
#include "oreslang/imports/ImportPreamble.hpp"
#include "oreslang/imports/ManifestSourceResolver.hpp"
#include "oreslang/frontend/SourceInput.hpp"
#include <functional>
#include <map>
#include <stdexcept>
#include <vector>
#include <utility>
#include <string>
#include <string_view>

namespace oreslang::imports {

struct ScalarImportUnit {
    fs::path file;
    std::size_t begin = 0;
    std::size_t end = 0;
    bool entry = false;
};
struct ScalarImportClosure {
    std::string source;
    std::vector<ScalarImportUnit> units;
};
inline ScalarImportClosure assemble_scalar_import_closure(
    const fs::path& entry,
    const std::function<void(std::string_view)>& validate_utf8) {
    constexpr std::size_t max_modules = 64;
    constexpr std::size_t max_depth = 32;
    constexpr std::size_t max_total_bytes = 8u * 1024u * 1024u;
    std::map<fs::path, unsigned> states;
    SourceIdentityRegistry identities;
    std::string combined;
    std::vector<ScalarImportUnit> units;
    const auto physical_entry = fs::canonical(entry);
    std::size_t total_bytes = 0;

    std::function<void(const fs::path&, const fs::path&, std::size_t)> visit;
    visit = [&](const fs::path& filename, const fs::path& boundary, std::size_t depth) {
        if (depth > max_depth)
            throw std::invalid_argument("native scalar import depth exceeded");
        const auto physical = fs::canonical(filename);
        if (physical.extension() != ".ores")
            throw std::invalid_argument("native compiler imports require physical .ores source");
        const auto project = read_project(physical, boundary);
        identities.register_source(project, physical);
        const auto known = states.find(physical);
        if (known != states.end()) {
            if (known->second == 1)
                throw std::invalid_argument("cyclic native scalar imports are unsupported");
            return;
        }
        if (states.size() >= max_modules)
            throw std::invalid_argument("native scalar import module count exceeded");
        states.emplace(physical, 1);
        const auto path = physical.string();
        const auto source = read_regular_source(path.c_str(), max_total_bytes,
            "native scalar source byte limit exceeded");
        validate_utf8(source);
        if (source.size() > max_total_bytes - total_bytes)
            throw std::invalid_argument("native scalar import closure exceeds 8 MiB");
        total_bytes += source.size();
        const auto preamble = split_import_preamble(source);
        for (const auto& raw : preamble.imports) {
            const auto input = classify(raw);
            if (input.kind == Kind::Absolute)
                throw std::invalid_argument("absolute native scalar imports are unsupported");
            const auto target = resolve_source(project, physical, raw);
            if (!target)
                throw std::invalid_argument("native scalar import source not found: " + raw);
            fs::path next_boundary = project.boundary;
            if (input.kind == Kind::Package) {
                const auto dependency = project.dependencies.at(input.alias);
                if (!beneath(fs::canonical(dependency), *target))
                    throw std::invalid_argument("package alias resolved outside installed dependency");
                next_boundary = dependency;
            } else if (project.package_manifest &&
                       (input.kind == Kind::Relative || input.kind == Kind::Root)) {
                if (!beneath(fs::canonical(project.root), *target))
                    throw std::invalid_argument("relative import escaped declared package root");
            }
            visit(*target, next_boundary, depth + 1);
        }
        // Dependency-first assembly allows the existing parser to perform
        // name/arity/ownership validation across the combined scalar program.
        // This intentionally does NOT implement namespaces, exports or cycles.
        if (combined.size() >= max_total_bytes ||
            preamble.stripped_source.size() >= max_total_bytes - combined.size())
            throw std::invalid_argument("assembled native scalar source too large");
        const auto begin = combined.size();
        combined += preamble.stripped_source;
        combined.push_back('\n');
        units.push_back(ScalarImportUnit{physical,begin,combined.size(),
                                         physical==physical_entry});
        states.at(physical) = 2;
    };
    visit(physical_entry, {}, 0);
    return {std::move(combined),std::move(units)};
}
} // namespace oreslang::imports
