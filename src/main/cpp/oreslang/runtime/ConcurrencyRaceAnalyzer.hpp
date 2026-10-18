#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Host-side vector-clock happens-before analyzer for fully instrumented traces.
 * This header is NOT wired into generated guest loads/stores yet. A clean report
 * does not establish race freedom until the compiler emits complete access and
 * synchronization events. It has no platform/OS dependency beyond C++17.
 */
class ConcurrencyRaceAnalyzer final {
 public:
  struct Finding {
    std::string location;
    std::string prior_participant;
    std::string current_participant;
    std::string conflict;
  };

  void read(const std::string& participant, const std::string& location) {
    std::lock_guard<std::mutex> hold(mu_);
    budget();
    const auto id = key(location);
    const auto access = tick(participant);
    auto& state = locations_[id];
    conflict(id, state.last_write, access, "write-read");
    state.reads[participant] = access;
  }

  void write(const std::string& participant, const std::string& location) {
    std::lock_guard<std::mutex> hold(mu_);
    budget();
    const auto id = key(location);
    const auto access = tick(participant);
    auto& state = locations_[id];
    conflict(id, state.last_write, access, "write-write");
    for (const auto& pair : state.reads) {
      conflict(id, std::optional<Access>(pair.second), access, "read-write");
    }
    state.reads.clear();
    state.last_write = access;
  }

  void send(const std::string& participant, const std::string& message_id) {
    std::lock_guard<std::mutex> hold(mu_);
    budget();
    const auto id = key(message_id);
    if (messages_.count(id) != 0) {
      throw std::logic_error("message token already published");
    }
    messages_.emplace(id, tick(participant).clock);
  }

  void receive(const std::string& participant, const std::string& message_id) {
    std::lock_guard<std::mutex> hold(mu_);
    budget();
    auto it = messages_.find(key(message_id));
    if (it == messages_.end()) {
      throw std::logic_error("message token not published");
    }
    merge(clock(participant), it->second);
    messages_.erase(it);
    tick(participant);
  }

  void release(const std::string& participant, const std::string& lock_id) {
    std::lock_guard<std::mutex> hold(mu_);
    budget();
    locks_[key(lock_id)] = tick(participant).clock;
  }

  void acquire(const std::string& participant, const std::string& lock_id) {
    std::lock_guard<std::mutex> hold(mu_);
    budget();
    const auto it = locks_.find(key(lock_id));
    if (it != locks_.end()) merge(clock(participant), it->second);
    tick(participant);
  }

  std::vector<Finding> findings() const {
    std::lock_guard<std::mutex> hold(mu_);
    return findings_;
  }

  std::size_t event_count() const {
    std::lock_guard<std::mutex> hold(mu_);
    return events_;
  }

 private:
  using Clock = std::map<std::string, std::uint64_t>;
  struct Access {
    std::string participant;
    Clock clock;
  };
  struct Location {
    std::optional<Access> last_write;
    std::unordered_map<std::string, Access> reads;
  };

  static std::string key(const std::string& value) {
    if (value.empty() || value.size() > 128 ||
        value.find_first_not_of(" \t\r\n") == std::string::npos) {
      throw std::invalid_argument("event identity must contain 1..128 characters");
    }
    return value;
  }

  void budget() {
    if (events_ == 100000) throw std::logic_error("race analyzer event budget exhausted");
    ++events_;
  }

  Clock& clock(const std::string& participant) {
    return clocks_[key(participant)];
  }

  static void merge(Clock& target, const Clock& source) {
    for (const auto& [id, value] : source) {
      target[id] = std::max(target[id], value);
    }
  }

  static bool happens_before(const Clock& earlier, const Clock& later) {
    for (const auto& [id, value] : earlier) {
      const auto it = later.find(id);
      if (value > (it == later.end() ? 0 : it->second)) return false;
    }
    return true;
  }

  Access tick(const std::string& participant) {
    Clock& current = clock(participant);
    auto& epoch = current[participant];
    if (epoch == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("vector clock epoch exhausted");
    }
    ++epoch;
    return Access{participant, current};
  }

  void conflict(const std::string& location, const std::optional<Access>& previous,
                const Access& current, const char* kind) {
    if (previous && !happens_before(previous->clock, current.clock) &&
        findings_.size() < 1000) {
      findings_.push_back(Finding{location, previous->participant,
                                  current.participant, kind});
    }
  }

  mutable std::mutex mu_;
  std::size_t events_ = 0;
  std::unordered_map<std::string, Clock> clocks_;
  std::unordered_map<std::string, Location> locations_;
  std::unordered_map<std::string, Clock> messages_;
  std::unordered_map<std::string, Clock> locks_;
  std::vector<Finding> findings_;
};

}  // namespace oreslang::runtime
