#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace oreslang::runtime {

// Native logical-invocation gate. The actor scheduler will own token lifecycle
// once LLVM actor lowering exists. No mutex remains held during guest execution,
// so a continuation can be resumed by a different OS thread.
class CriticalGate final {
public:
    using Token = std::uint64_t;
    enum class Admission { acquired, queued };
    static constexpr std::size_t max_waiters = 8192;

    Admission admit(Token token) {
        if (token == 0) throw std::invalid_argument("critical token zero is invalid");
        std::lock_guard<std::mutex> lock(mu_);
        if (owner_ == token || is_queued(token)) {
            throw std::logic_error("critical ticket admitted more than once");
        }
        if (owner_ == 0 && waiting_.empty()) {
            owner_ = token;
            return Admission::acquired;
        }
        if (waiting_.size() >= max_waiters) {
            throw std::overflow_error("critical waiter limit exceeded");
        }
        waiting_.push_back(token);
        return Admission::queued;
    }

    // Release/cancel are idempotent. Return the newly admitted token, or zero.
    // Scheduler must enqueue its continuation, never run guest code inline.
    Token release(Token token) {
        if (token == 0) throw std::invalid_argument("critical token zero is invalid");
        std::lock_guard<std::mutex> lock(mu_);
        if (owner_ != token) {
            for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
                if (*it == token) {
                    waiting_.erase(it);
                    return 0;
                }
            }
            return 0;
        }
        owner_ = 0;
        if (waiting_.empty()) return 0;
        owner_ = waiting_.front();
        waiting_.pop_front();
        return owner_;
    }

    bool holds(Token token) const {
        std::lock_guard<std::mutex> lock(mu_);
        return token != 0 && owner_ == token;
    }

    std::size_t waiting() const {
        std::lock_guard<std::mutex> lock(mu_);
        return waiting_.size();
    }

private:
    bool is_queued(Token token) const {
        for (Token pending : waiting_) if (pending == token) return true;
        return false;
    }

    mutable std::mutex mu_;
    Token owner_ = 0;
    std::deque<Token> waiting_;
};

} // namespace oreslang::runtime
