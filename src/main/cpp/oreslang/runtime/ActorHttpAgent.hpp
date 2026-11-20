#pragma once
// HTTP/1.1 client agent over ActorSharedTcpPool, safe to share across actors.
//
// The agent holds no per-actor state; the caller's scheduler-authenticated
// PoolActor is passed on every call. Cross-actor safety on a reused
// connection relies on three rules enforced here:
//  1. The agent serializes the request itself from a structured spec and
//     rejects CR/LF/NUL and framing headers. An actor therefore cannot
//     pipeline a second request whose response would later be read by
//     another actor on the same connection.
//  2. Responses are framed strictly (Content-Length XOR chunked, no
//     duplicates that disagree, no bare LF, no obs-fold). Anything ambiguous
//     is a protocol error and the connection is closed, not pooled.
//  3. The connection is pooled only if the response ended exactly at the
//     framing boundary with no extra bytes, keep-alive was not refused, and
//     no error, timeout or revocation happened during the exchange.
// Response bytes are copied into a buffer owned by the caller's result; no
// buffer is shared between actors.
#include "ActorSharedTcpPool.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace oreslang::runtime {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequestSpec {
    std::string method = "GET";
    std::string target = "/";
    std::string host;
    std::vector<HttpHeader> headers;
    std::vector<std::uint8_t> body;
};

struct HttpResponseOwned {
    int status = 0;
    std::vector<HttpHeader> headers;
    std::vector<std::uint8_t> body;
    bool keep_alive = false;

    const std::string* header(std::string_view name) const {
        for (const auto& h : headers)
            if (h.name.size() == name.size()
                    && std::equal(name.begin(), name.end(), h.name.begin(),
                                  [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
                return &h.value;
        return nullptr;
    }
};

struct HttpAgentResult {
    PoolStatus status = PoolStatus::io_error;
    HttpResponseOwned response;
    bool reused_connection = false;
    bool pooled_after = false;
    std::uint32_t attempts = 0;
};

namespace http_detail {

inline bool ieq(std::string_view a, std::string_view b) {
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
           });
}

inline bool is_tchar(unsigned char c) {
    return std::isalnum(c) || std::string_view("!#$%&'*+-.^_`|~").find((char)c) != std::string_view::npos;
}

inline bool is_token(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return is_tchar((unsigned char)c); });
}

inline std::string_view trim_ows(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Field values: visible ASCII, SP, HTAB, obs-text. No CTLs (CR, LF, NUL...).
inline bool is_field_value(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char ch) {
        const auto c = (unsigned char)ch;
        return c == '\t' || (c >= 0x20 && c != 0x7f);
    });
}

inline bool has_token(std::string_view list, std::string_view token) {
    while (!list.empty()) {
        const auto comma = list.find(',');
        if (ieq(trim_ows(list.substr(0, comma)), token)) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

inline bool is_reserved_request_header(std::string_view name) {
    static constexpr std::string_view reserved[] = {
        "host", "content-length", "transfer-encoding", "connection", "keep-alive",
        "upgrade", "te", "trailer", "expect", "proxy-connection", "http2-settings"};
    return std::any_of(std::begin(reserved), std::end(reserved), [&](std::string_view r) { return ieq(r, name); });
}

inline bool is_idempotent(std::string_view m) {
    return m == "GET" || m == "HEAD" || m == "OPTIONS" || m == "PUT" || m == "DELETE";
}

// Returns false (bad_request) instead of producing ambiguous bytes.
inline bool serialize_request(const HttpRequestSpec& r, std::string& out) {
    static constexpr std::string_view methods[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"};
    if (std::none_of(std::begin(methods), std::end(methods), [&](std::string_view m) { return m == r.method; }))
        return false;
    if (r.target.empty() || r.target.front() != '/') return false;
    auto visible = [](std::string_view s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return (unsigned char)c > 0x20 && (unsigned char)c < 0x7f; });
    };
    if (!visible(r.target) || !visible(r.host)) return false;
    if (r.method == "HEAD" && !r.body.empty()) return false;
    out.clear();
    out.reserve(128 + r.body.size());
    out += r.method; out += ' '; out += r.target; out += " HTTP/1.1\r\nHost: "; out += r.host; out += "\r\n";
    for (const auto& h : r.headers) {
        if (!is_token(h.name) || !is_field_value(h.value) || is_reserved_request_header(h.name)) return false;
        out += h.name; out += ": "; out += trim_ows(h.value); out += "\r\n";
    }
    if (!r.body.empty() || r.method == "POST" || r.method == "PUT" || r.method == "PATCH") {
        out += "Content-Length: "; out += std::to_string(r.body.size()); out += "\r\n";
    }
    out += "\r\n";
    out.append(reinterpret_cast<const char*>(r.body.data()), r.body.size());
    return true;
}

// Incremental, strict HTTP/1.1 response framer. Never rescans consumed bytes.
class ResponseFramer final {
public:
    enum class Step { need_more, done, error };

    ResponseFramer(bool head_request, std::size_t max_header, std::size_t max_body)
        : head_(head_request), max_header_(max_header), max_body_(max_body) {}

    void append(const std::uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }

    Step advance() {
        for (;;) {
            switch (phase_) {
                case Phase::head: {
                    const auto end = find_crlfcrlf();
                    if (end == npos) {
                        if (buf_.size() - pos_ > max_header_) return fail(PoolStatus::too_large);
                        // Fail fast on a bare LF instead of waiting for a CRLFCRLF that never comes.
                        for (std::size_t i = std::max(lf_checked_, pos_); i < buf_.size(); ++i)
                            if (buf_[i] == '\n' && (i == pos_ || buf_[i - 1] != '\r')) return fail(PoolStatus::protocol_error);
                        lf_checked_ = buf_.size();
                        return Step::need_more;
                    }
                    if (end + 4 - pos_ > max_header_) return fail(PoolStatus::too_large);
                    if (!parse_head(std::string_view(reinterpret_cast<const char*>(buf_.data()) + pos_, end - pos_)))
                        return fail(error_ == PoolStatus::ok ? PoolStatus::protocol_error : error_);
                    pos_ = end + 4;
                    scan_ = pos_;
                    if (phase_ == Phase::head) continue;  // interim 1xx consumed
                    break;
                }
                case Phase::length: {
                    const std::size_t take = std::min<std::size_t>(remaining_, buf_.size() - pos_);
                    if (!take_body(take)) return fail(PoolStatus::too_large);
                    remaining_ -= take;
                    if (remaining_ != 0) return Step::need_more;
                    phase_ = Phase::done;
                    break;
                }
                case Phase::chunk_size: {
                    const auto eol = find_crlf();
                    if (eol == npos) return buf_.size() - pos_ > 64 ? fail(PoolStatus::protocol_error) : Step::need_more;
                    std::string_view line(reinterpret_cast<const char*>(buf_.data()) + pos_, eol - pos_);
                    line = line.substr(0, line.find(';'));
                    line = trim_ows(line);
                    if (line.empty() || line.size() > 15) return fail(PoolStatus::protocol_error);
                    std::uint64_t size = 0;
                    for (char c : line) {
                        const int d = std::isdigit((unsigned char)c) ? c - '0'
                                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                        if (d < 0) return fail(PoolStatus::protocol_error);
                        size = size * 16 + static_cast<std::uint64_t>(d);
                    }
                    pos_ = eol + 2;
                    scan_ = pos_;
                    if (size == 0) { phase_ = Phase::trailers; break; }
                    if (size > max_body_ - response.body.size()) return fail(PoolStatus::too_large);
                    remaining_ = size;
                    phase_ = Phase::chunk_data;
                    break;
                }
                case Phase::chunk_data: {
                    const std::size_t take = std::min<std::size_t>(remaining_, buf_.size() - pos_);
                    if (!take_body(take)) return fail(PoolStatus::too_large);
                    remaining_ -= take;
                    if (remaining_ != 0) return Step::need_more;
                    phase_ = Phase::chunk_crlf;
                    break;
                }
                case Phase::chunk_crlf: {
                    if (buf_.size() - pos_ < 2) return Step::need_more;
                    if (buf_[pos_] != '\r' || buf_[pos_ + 1] != '\n') return fail(PoolStatus::protocol_error);
                    pos_ += 2;
                    scan_ = pos_;
                    phase_ = Phase::chunk_size;
                    break;
                }
                case Phase::trailers: {
                    const auto eol = find_crlf();
                    if (eol == npos) return buf_.size() - pos_ > max_header_ ? fail(PoolStatus::too_large) : Step::need_more;
                    const bool empty = eol == pos_;
                    pos_ = eol + 2;
                    scan_ = pos_;
                    if (empty) phase_ = Phase::done;
                    break;
                }
                case Phase::until_eof: {
                    if (!take_body(buf_.size() - pos_)) return fail(PoolStatus::too_large);
                    return Step::need_more;
                }
                case Phase::done: return Step::done;
                case Phase::error: return Step::error;
            }
        }
    }

    Step on_eof() {
        if (phase_ == Phase::until_eof) { phase_ = Phase::done; return Step::done; }
        if (phase_ == Phase::done) return Step::done;
        return fail(PoolStatus::peer_closed);
    }

    bool got_bytes() const { return !buf_.empty(); }
    std::size_t leftover() const { return buf_.size() - pos_; }
    PoolStatus error() const { return error_; }

    HttpResponseOwned response;

private:
    enum class Phase { head, length, chunk_size, chunk_data, chunk_crlf, trailers, until_eof, done, error };
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    Step fail(PoolStatus s) { phase_ = Phase::error; error_ = s; return Step::error; }

    bool take_body(std::size_t n) {
        if (n > max_body_ - response.body.size()) return false;
        response.body.insert(response.body.end(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_),
                             buf_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        scan_ = pos_;
        return true;
    }

    std::size_t find_crlf() {
        for (std::size_t i = std::max(scan_, pos_); i + 1 < buf_.size(); ++i)
            if (buf_[i] == '\r' && buf_[i + 1] == '\n') return i;
        scan_ = buf_.size() > 0 ? buf_.size() - 1 : 0;
        return npos;
    }

    std::size_t find_crlfcrlf() {
        for (std::size_t i = std::max(scan_, pos_); i + 3 < buf_.size(); ++i)
            if (buf_[i] == '\r' && buf_[i + 1] == '\n' && buf_[i + 2] == '\r' && buf_[i + 3] == '\n') return i;
        scan_ = buf_.size() >= 3 ? std::max(pos_, buf_.size() - 3) : pos_;
        return npos;
    }

    bool parse_head(std::string_view head) {
        // Reject bare CR / bare LF anywhere: CRLF is the only line terminator.
        for (std::size_t i = 0; i < head.size(); ++i) {
            if (head[i] == '\n' && (i == 0 || head[i - 1] != '\r')) return false;
            if (head[i] == '\r' && (i + 1 >= head.size() || head[i + 1] != '\n')) return false;
        }
        auto next_line = [&](std::string_view& rest) {
            const auto e = rest.find("\r\n");
            std::string_view line = rest.substr(0, e);
            rest = e == std::string_view::npos ? std::string_view{} : rest.substr(e + 2);
            return line;
        };
        std::string_view rest = head;
        const std::string_view status_line = next_line(rest);
        bool http11;
        if (status_line.substr(0, 9) == "HTTP/1.1 ") http11 = true;
        else if (status_line.substr(0, 9) == "HTTP/1.0 ") http11 = false;
        else return false;
        if (status_line.size() < 12 || (status_line.size() > 12 && status_line[12] != ' ')) return false;
        int code = 0;
        for (int i = 9; i < 12; ++i) {
            if (!std::isdigit((unsigned char)status_line[i])) return false;
            code = code * 10 + (status_line[i] - '0');
        }
        if (code < 100 || code > 599) return false;

        std::vector<HttpHeader> headers;
        bool has_cl = false, chunked = false, conn_close = false;
        std::uint64_t content_length = 0;
        while (!rest.empty()) {
            const std::string_view line = next_line(rest);
            if (line.empty()) return false;
            if (line.front() == ' ' || line.front() == '\t') return false;  // obs-fold
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) return false;
            const std::string_view name = line.substr(0, colon);
            const std::string_view value = trim_ows(line.substr(colon + 1));
            if (!is_token(name) || !is_field_value(value)) return false;
            if (ieq(name, "content-length")) {
                if (value.empty() || value.size() > 18
                        || !std::all_of(value.begin(), value.end(), [](char c) { return std::isdigit((unsigned char)c); }))
                    return false;
                std::uint64_t v = 0;
                for (char c : value) v = v * 10 + static_cast<std::uint64_t>(c - '0');
                if (has_cl && v != content_length) return false;
                has_cl = true;
                content_length = v;
            } else if (ieq(name, "transfer-encoding")) {
                if (chunked || !ieq(value, "chunked")) return false;
                chunked = true;
            } else if (ieq(name, "connection")) {
                if (has_token(value, "close")) conn_close = true;
            }
            headers.push_back({std::string(name), std::string(value)});
        }
        if (has_cl && chunked) return false;  // request-smuggling ambiguity

        if (code >= 100 && code < 200) {
            if (code == 101 || ++interim_ > 8) return false;  // no upgrades on pooled connections
            return true;  // stay in Phase::head for the final response
        }
        response.status = code;
        response.headers = std::move(headers);
        response.keep_alive = http11 && !conn_close;
        if (head_ || code == 204 || code == 304) {
            phase_ = Phase::done;
        } else if (chunked) {
            phase_ = Phase::chunk_size;
        } else if (has_cl) {
            if (content_length > max_body_) { error_ = PoolStatus::too_large; return false; }
            remaining_ = content_length;
            phase_ = remaining_ == 0 ? Phase::done : Phase::length;
        } else {
            response.keep_alive = false;  // close-delimited body
            phase_ = Phase::until_eof;
        }
        return true;
    }

    bool head_;
    std::size_t max_header_, max_body_;
    std::vector<std::uint8_t> buf_;
    std::size_t pos_ = 0, scan_ = 0, lf_checked_ = 0;
    std::uint64_t remaining_ = 0;
    int interim_ = 0;
    Phase phase_ = Phase::head;
    PoolStatus error_ = PoolStatus::ok;

};

inline int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(std::min<long long>(left, 60'000));
}

inline PoolStatus wait_fd(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        pollfd p{fd, events, 0};
        const int ms = remaining_ms(deadline);
        if (ms == 0) return PoolStatus::timeout;
        const int r = ::poll(&p, 1, ms);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return PoolStatus::io_error;
        if (r == 0) return PoolStatus::timeout;
        return PoolStatus::ok;
    }
}

inline PoolStatus write_all(int fd, const char* p, std::size_t n, std::chrono::steady_clock::time_point deadline) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;  // SO_NOSIGPIPE set at connect
#endif
    while (n > 0) {
        const ssize_t w = ::send(fd, p, n, flags);
        if (w > 0) { p += w; n -= static_cast<std::size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (const auto st = wait_fd(fd, POLLOUT, deadline); st != PoolStatus::ok) return st;
            continue;
        }
        return (errno == EPIPE || errno == ECONNRESET) ? PoolStatus::peer_closed : PoolStatus::io_error;
    }
    return PoolStatus::ok;
}

}  // namespace http_detail

class ActorHttpAgent final {
public:
    explicit ActorHttpAgent(ActorSharedTcpPool& pool) : pool_(pool) {}

    // One logical request. At most one transparent retry, and only for an
    // idempotent request whose reused connection failed before any response
    // byte arrived (the server closed a keep-alive connection we just took).
    HttpAgentResult request(PoolActor caller, PoolOrigin origin, const HttpRequestSpec& spec) {
        HttpAgentResult result;
        std::string wire;
        if (!http_detail::serialize_request(spec, wire)) { result.status = PoolStatus::bad_request; return result; }
        const bool head = spec.method == "HEAD";
        for (std::uint32_t attempt = 1; attempt <= 2; ++attempt) {
            result = HttpAgentResult{};
            result.attempts = attempt;
            const auto acquired = pool_.acquire(caller, origin);
            if (acquired.status != PoolStatus::ok) { result.status = acquired.status; return result; }
            result.reused_connection = acquired.lease.reused;
            bool got_bytes = false;
            const PoolStatus io = pool_.with_fd(PoolIoKey{}, caller, acquired.lease, [&](int fd) {
                return exchange(fd, wire, head, result, got_bytes);
            });
            if (io != PoolStatus::ok) result.status = io;
            const bool reusable = result.status == PoolStatus::ok && result.response.keep_alive;
            const PoolStatus rel = pool_.release(caller, acquired.lease, reusable);
            result.pooled_after = reusable && rel == PoolStatus::ok;
            // The lease vanished under us: the actor was revoked or the pool
            // shut down. Report it and never retry on behalf of a dead actor.
            if (rel == PoolStatus::invalid_lease || io == PoolStatus::invalid_lease) {
                result.status = PoolStatus::revoked;
                result.pooled_after = false;
                return result;
            }
            const bool retry = attempt == 1 && acquired.lease.reused && !got_bytes
                && http_detail::is_idempotent(spec.method)
                && (result.status == PoolStatus::peer_closed || result.status == PoolStatus::io_error);
            if (!retry) return result;
        }
        return result;
    }

private:
    bool exchange(int fd, const std::string& wire, bool head, HttpAgentResult& out, bool& got_bytes) {
        const auto& cfg = pool_.config();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg.io_timeout_ms);
        out.status = http_detail::write_all(fd, wire.data(), wire.size(), deadline);
        if (out.status != PoolStatus::ok) return false;
        http_detail::ResponseFramer framer(head, cfg.max_header_bytes, cfg.max_response_bytes);
        std::uint8_t chunk[16 * 1024];
        for (;;) {
            auto step = framer.advance();
            if (step == http_detail::ResponseFramer::Step::need_more) {
                const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
                if (n > 0) { framer.append(chunk, static_cast<std::size_t>(n)); got_bytes = true; continue; }
                if (n == 0) { step = framer.on_eof(); }
                else if (errno == EINTR) continue;
                else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    out.status = http_detail::wait_fd(fd, POLLIN, deadline);
                    if (out.status != PoolStatus::ok) return false;
                    continue;
                } else {
                    out.status = errno == ECONNRESET ? PoolStatus::peer_closed : PoolStatus::io_error;
                    return false;
                }
                if (step == http_detail::ResponseFramer::Step::need_more) continue;
            }
            if (step == http_detail::ResponseFramer::Step::error) {
                out.status = framer.error();
                return false;
            }
            break;  // done
        }
        out.response = std::move(framer.response);
        out.status = PoolStatus::ok;
        // Bytes beyond this response's framing belong to nobody. Never pool them.
        if (framer.leftover() != 0) out.response.keep_alive = false;
        return out.response.keep_alive;
    }

    ActorSharedTcpPool& pool_;
};

}  // namespace oreslang::runtime
