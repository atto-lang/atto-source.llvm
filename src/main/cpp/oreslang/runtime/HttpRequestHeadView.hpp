#pragma once
// Host-only, strict-subset, allocation-free HTTP/1.x request-head view.
// Metadata parsing is NOT body/framing safety or guest lowering.
// All string_views borrow bytes and must not outlive the owning ReadLease.
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>

namespace oreslang::runtime {
class HttpRequestHeadView final {
    std::string_view raw_;
    std::string_view method_;
    std::string_view target_;
    std::string_view version_;
    std::size_t headers_offset_ = 0;
    std::size_t header_end_ = 0;
    std::size_t header_count_ = 0;
    std::size_t supplied_bytes_ = 0;
    static bool name_char(unsigned char c) noexcept {
        // Strict supported subset of RFC tchar; reject unsupported spellings.
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-';
    }
    static bool eql_ci(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            unsigned char x = static_cast<unsigned char>(a[i]);
            unsigned char y = static_cast<unsigned char>(b[i]);
            if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x + ('a'-'A'));
            if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y + ('a'-'A'));
            if (x != y) return false;
        }
        return true;
    }
public:
    static constexpr std::size_t max_head_bytes = 32768;
    static constexpr std::size_t max_header_count = 100;
    static std::optional<HttpRequestHeadView> parse(std::string_view bytes) noexcept {
        const auto head = bytes.substr(0, std::min(bytes.size(), max_head_bytes));
        const auto line_end = head.find("\r\n");
        if (line_end == std::string_view::npos || line_end == 0) return std::nullopt;
        const auto line = head.substr(0, line_end);
        const auto s1 = line.find(' ');
        if (s1 == std::string_view::npos || s1 == 0 || s1 > 32) return std::nullopt;
        const auto s2 = line.find(' ', s1 + 1);
        if (s2 == std::string_view::npos || s2 == s1 + 1) return std::nullopt;
        const auto method = line.substr(0, s1);
        const auto target = line.substr(s1 + 1, s2 - s1 - 1);
        const auto version = line.substr(s2 + 1);
        if (version != "HTTP/1.1" && version != "HTTP/1.0") return std::nullopt;
        if (target.size() > 8192) return std::nullopt;
        for (unsigned char c : method) if (c < 'A' || c > 'Z') return std::nullopt;
        for (unsigned char c : target) if (c <= 0x20 || c == 0x7f) return std::nullopt;
        HttpRequestHeadView result;
        result.raw_ = head;
        result.supplied_bytes_ = bytes.size();
        result.method_ = method;
        result.target_ = target;
        result.version_ = version;
        result.headers_offset_ = line_end + 2;
        std::size_t pos = result.headers_offset_;
        while (pos < head.size()) {
            const auto end = head.find("\r\n", pos);
            if (end == std::string_view::npos) return std::nullopt;
            if (pos == end) {
                result.header_end_ = end + 2;
                return result;
            }
            if (++result.header_count_ > max_header_count) return std::nullopt;
            const auto line_header = head.substr(pos, end - pos);
            const auto colon = line_header.find(':');
            if (colon == std::string_view::npos || colon == 0) return std::nullopt;
            for (std::size_t i = 0; i < colon; ++i)
                if (!name_char(static_cast<unsigned char>(line_header[i]))) return std::nullopt;
            // Strict C0/DEL rejection; obs-fold rejected via invalid field-name.
            for (std::size_t i = colon + 1; i < line_header.size(); ++i) {
                const auto c = static_cast<unsigned char>(line_header[i]);
                if ((c < 0x20 && c != '\t') || c == 0x7f) return std::nullopt;
            }
            pos = end + 2;
        }
        return std::nullopt;
    }
    std::string_view method() const noexcept { return method_; }
    std::string_view target() const noexcept { return target_; }
    std::string_view version() const noexcept { return version_; }
    std::size_t header_count() const noexcept { return header_count_; }
    std::size_t head_bytes() const noexcept { return header_end_; }
    // Fail-closed fast-path admission for a COMPLETE, unframed GET/HEAD
    // request. Does not interpret transfer codings or streaming bodies.
    // Rejecting those cases here avoids accidentally treating first_header()
    // as a body-framing decision. A production proxy must still implement
    // full RFC 9112 validation, connection state and timeout budgets.
    bool safe_unframed_request() const noexcept {
        if (header_end_ == 0 || header_end_ != supplied_bytes_) return false;
        if (method_ != "GET" && method_ != "HEAD") return false;
        std::size_t host_count = 0;
        std::size_t length_count = 0;
        auto pos = headers_offset_;
        while (pos < header_end_ - 2) {
            const auto end = raw_.find("\r\n", pos);
            if (end == std::string_view::npos) return false;
            const auto line = raw_.substr(pos, end - pos);
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) return false;
            const auto name = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            if (eql_ci(name, "Host")) {
                if (++host_count != 1 || value.empty()) return false;
                // Deliberately narrow authority syntax: ASCII hostname,
                // numeric port or bracketed IPv6. Never forward commas,
                // userinfo, paths or other proxy-ambiguous host forms.
                for (unsigned char c : value) {
                    const bool alpha = (c >= 'a' && c <= 'z') ||
                                       (c >= 'A' && c <= 'Z');
                    const bool digit = c >= '0' && c <= '9';
                    if (!alpha && !digit && c != '-' && c != '.' &&
                        c != ':' && c != '[' && c != ']') return false;
                }
                if (value.front() == ':' || value.front() == '-' ||
                    value.front() == '.' || value.front() == ']') return false;
            } else if (eql_ci(name, "Content-Length")) {
                // Deliberately stricter than RFC: only canonical zero and no
                // duplicates, never a length list or partial request body.
                if (++length_count != 1 || value != "0") return false;
            } else if (eql_ci(name, "Transfer-Encoding")
                    || eql_ci(name, "Expect")
                    || eql_ci(name, "Upgrade")) {
                return false;
            }
            pos = end + 2;
        }
        return version_ == "HTTP/1.0" || host_count == 1;
    }
    // NOT a framing resolver for duplicate CL/TE or HTTP request smuggling.
    std::optional<std::string_view> first_header(std::string_view key) const noexcept {
        auto pos = headers_offset_;
        while (pos < header_end_ - 2) {
            auto end = raw_.find("\r\n", pos);
            auto line = raw_.substr(pos, end - pos);
            auto colon = line.find(':');
            if (eql_ci(line.substr(0, colon), key)) {
                auto value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.remove_prefix(1);
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                    value.remove_suffix(1);
                return value;
            }
            pos = end + 2;
        }
        return std::nullopt;
    }
};
} // namespace oreslang::runtime
