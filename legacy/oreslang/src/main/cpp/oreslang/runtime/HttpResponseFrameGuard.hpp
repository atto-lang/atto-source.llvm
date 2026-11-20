#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace oreslang {

/**
 * Native host-side guard for HTTP response fragments emitted by an actor
 * over its data-only output stream. Only the trusted supervisor owns the FD.
 *
 * This is a bounds/authentication/state-machine substrate, NOT the wire
 * decoder, source-level HttpResponseFrame lowering or a socket writer.
 */
class HttpResponseFrameGuard final {
public:
    enum class State : std::uint8_t { New, Body, Ended, Aborted };
    enum class Admission : std::uint8_t {
        Ok, Unauthorized, StaleSequence, InvalidOrder, InvalidHeader, InvalidStatus,
        InvalidBody, CreditExceeded, QuotaExceeded
    };

    HttpResponseFrameGuard(
            std::string actor_id,
            std::string request_id,
            std::size_t max_chunk,
            std::size_t max_total)
        : actor_id_(std::move(actor_id)),
          request_id_(std::move(request_id)),
          max_chunk_(max_chunk),
          max_total_(max_total) { }

    bool valid() const noexcept {
        return valid_id(actor_id_) && valid_id(request_id_)
                && max_chunk_ != 0 && max_chunk_ <= 1'048'576 && max_total_ != 0;
    }

    // Only the trusted parent may grant more credit after downstream
    // capacity has been reserved. No blocking writes on an actor carrier.
    Admission grant_credit(std::size_t bytes) noexcept {
        if (!valid() || state_ == State::Ended || state_ == State::Aborted
                || bytes == 0 || bytes > max_total_
                || credit_ > max_total_ - bytes) return Admission::QuotaExceeded;
        credit_ += bytes;
        return Admission::Ok;
    }

    Admission begin(
            std::string_view actor,
            std::string_view request,
            std::uint64_t sequence,
            unsigned status) noexcept {
        const Admission base = admit(actor, request, sequence);
        if (base != Admission::Ok) return base;
        if (state_ != State::New) return Admission::InvalidOrder;
        if (status < 200 || status > 599) return Admission::InvalidStatus;
        state_ = State::Body;
        body_allowed_ = status != 204 && status != 205 && status != 304;
        commit(sequence);
        return Admission::Ok;
    }

    // The parent should validate all header names/values before performing
    // a socket write. No content-length, transfer-encoding or hop-by-hop
    // response framing is delegated to the guest actor.
    static bool valid_guest_header(
            std::string_view name,
            std::string_view value) noexcept {
        if (value.size() > 2048) return false;
        for (char c : value) {
            if (c == '\r' || c == '\n' || c == '\0') return false;
        }
        return name == "content-type" || name == "cache-control"
                || name == "etag" || name == "vary"
                || name == "content-language"
                || name == "last-modified" || name == "expires";
    }

    Admission chunk(
            std::string_view actor,
            std::string_view request,
            std::uint64_t sequence,
            std::size_t encoded_bytes) noexcept {
        const Admission base = admit(actor, request, sequence);
        if (base != Admission::Ok) return base;
        if (state_ != State::Body || !body_allowed_) return Admission::InvalidOrder;
        if (encoded_bytes > max_chunk_) return Admission::InvalidBody;
        if (encoded_bytes > credit_) return Admission::CreditExceeded;
        if (encoded_bytes > max_total_ - used_) return Admission::QuotaExceeded;
        used_ += encoded_bytes;
        credit_ -= encoded_bytes;
        commit(sequence);
        return Admission::Ok;
    }

    Admission end(
            std::string_view actor,
            std::string_view request,
            std::uint64_t sequence) noexcept {
        const Admission base = admit(actor, request, sequence);
        if (base != Admission::Ok) return base;
        if (state_ != State::Body) return Admission::InvalidOrder;
        state_ = State::Ended;
        commit(sequence);
        return Admission::Ok;
    }

    void abort() noexcept {
        if (state_ != State::Ended) state_ = State::Aborted;
    }

    State state() const noexcept { return state_; }
    std::size_t total_bytes() const noexcept { return used_; }
    std::size_t remaining_credit() const noexcept { return credit_; }

private:
    static bool valid_id(std::string_view id) noexcept {
        if (id.empty() || id.size() > 128) return false;
        for (char c : id) {
            if (!((c >= '0' && c <= '9')
                    || (c >= 'A' && c <= 'Z')
                    || (c >= 'a' && c <= 'z')
                    || c == '-' || c == '_' || c == '.' || c == ':')) return false;
        }
        return true;
    }

    Admission admit(
            std::string_view actor,
            std::string_view request,
            std::uint64_t sequence) const noexcept {
        if (!valid() || actor != std::string_view(actor_id_) || request != std::string_view(request_id_)) {
            return Admission::Unauthorized;
        }
        if (has_sequence_ && sequence <= last_sequence_) {
            return Admission::StaleSequence;
        }
        return Admission::Ok;
    }

    void commit(std::uint64_t sequence) noexcept {
        has_sequence_ = true;
        last_sequence_ = sequence;
    }

    std::string actor_id_;
    std::string request_id_;
    std::size_t max_chunk_;
    std::size_t max_total_;
    std::size_t credit_ = 0;
    std::size_t used_ = 0;
    std::uint64_t last_sequence_ = 0;
    State state_ = State::New;
    bool has_sequence_ = false;
    bool body_allowed_ = true;
};
} // namespace oreslang
