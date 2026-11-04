#pragma once
// Source-owned manifest snapshot for native .atto package graphs.
// Captures and rechecks the same bounded, no-follow source identity as SourceInput.
// This is a best-effort race detector, not an atomic multi-file filesystem snapshot.
#include "oreslang/frontend/ProjectRootImport.hpp"
#include "oreslang/frontend/SourceInput.hpp"
#include <filesystem>
#include <stdexcept>
#include <string>

namespace oreslang::frontend {

class AttoManifestSnapshot {
    std::filesystem::path path_;
    SourceIdentity identity_{};
    std::string bytes_;
    static constexpr std::size_t max_manifest_bytes = 64u * 1024u;
    static constexpr const char* overflow =
        "native package manifest exceeds 64 KiB";
public:
    explicit AttoManifestSnapshot(std::filesystem::path path)
        : path_(std::move(path)) {
        const auto pathname = path_.string();
        const auto before = capture_source_identity(
            pathname.c_str(), max_manifest_bytes, overflow);
        bytes_ = read_regular_source(pathname.c_str(), max_manifest_bytes,
                                    overflow, &identity_, &before);
        require_valid_source_utf8(bytes_);
        project_root_import_detail::validate_manifest(bytes_);
    }

    void recheck() const {
        const auto pathname = path_.string();
        SourceIdentity observed{};
        try {
            const auto prior = capture_source_identity(
                pathname.c_str(), max_manifest_bytes, overflow);
            if (!ores_source_identity_equal(identity_, prior))
                throw std::runtime_error("ATTO_UNIT_MANIFEST_CHANGED");
            const auto now = read_regular_source(
                pathname.c_str(), max_manifest_bytes, overflow,
                &observed, &prior);
            if (!ores_source_identity_equal(identity_, observed) || now != bytes_)
                throw std::runtime_error("ATTO_UNIT_MANIFEST_CHANGED");
        } catch (const std::exception&) {
            // Fail closed if the manifest disappears, changes permissions,
            // swaps its descriptor, grows, or becomes a symlink.
            throw std::runtime_error("ATTO_UNIT_MANIFEST_CHANGED");
        }
    }
};
} // namespace oreslang::frontend
