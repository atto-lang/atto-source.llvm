#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_set>
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
    for (const auto& writer : state.writes) {
      conflict(id, std::optional<Access>(writer.second), access, "write-read");
    }
    state.reads[participant] = access;
  }

  void write(const std::string& participant, const std::string& location) {
    std::lock_guard<std::mutex> hold(mu_);
    budget();
    const auto id = key(location);
    const auto access = tick(participant);
    auto& state = locations_[id];
    for (const auto& writer : state.writes) {
      conflict(id, std::optional<Access>(writer.second), access, "write-write");
    }
    for (const auto& pair : state.reads) {
      conflict(id, std::optional<Access>(pair.second), access, "read-write");
    }
    // Do not erase other participants' histories when one writer commits.
    state.writes[participant] = access;
  }

  void send(const std::string& participant, const std::string& message_id) {
    std::lock_guard<std::mutex> hold(mu_);
    budget();
    const auto id = key(message_id);
    if (!issued_message_ids_.insert(id).second) {
      throw std::logic_error("message token cannot be reused, including after receive");
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

  // Reentrant ownership follows logical participant identity, not host threads.
  // Only the outermost unlock publishes synchronization to another owner.
  void release(const std::string& participant, const std::string& lock_id) {
    std::lock_guard<std::mutex> hold(mu_);
    const auto id = key(lock_id);
    const auto actor = key(participant);
    const auto owner = lock_owners_.find(id);
    if (owner == lock_owners_.end() || owner->second != actor) {
      throw std::logic_error("release requires the current lock owner");
    }
    budget();
    const auto depth = lock_depths_.at(id);
    if (depth > 1) {
      tick(actor);
      lock_depths_.at(id) = depth - 1;
    } else {
      locks_[id] = tick(actor).clock;
      lock_owners_.erase(owner);
      lock_depths_.erase(id);
    }
  }

  void acquire(const std::string& participant, const std::string& lock_id) {
    std::lock_guard<std::mutex> hold(mu_);
    const auto id = key(lock_id);
    const auto actor = key(participant);
    const auto owner = lock_owners_.find(id);
    if (owner != lock_owners_.end() && owner->second != actor) {
      throw std::logic_error("lock is already acquired by another participant");
    }
    if (owner != lock_owners_.end() &&
        lock_depths_.at(id) == std::numeric_limits<std::uint32_t>::max()) {
      throw std::overflow_error("reentrant lock depth exhausted");
    }
    budget();
    if (owner != lock_owners_.end()) {
      tick(actor);
      ++lock_depths_.at(id);
    } else {
      const auto it = locks_.find(id);
      if (it != locks_.end()) merge(clock(actor), it->second);
      tick(actor);
      lock_owners_[id] = actor;
      lock_depths_[id] = 1;
    }
  }

  std::vector<Finding> findings() const {
    std::lock_guard<std::mutex> hold(mu_);
    return findings_;
  }

  std::size_t event_count() const {
    std::lock_guard<std::mutex> hold(mu_);
    return events_;
  }

  std::uint64_t conflict_count() const {
    std::lock_guard<std::mutex> hold(mu_);
    return conflicts_;
  }

 private:
  using Clock = std::map<std::string, std::uint64_t>;
  struct Access {
    std::string participant;
    Clock clock;
  };
  struct Location {
    std::unordered_map<std::string, Access> writes;
    std::unordered_map<std::string, Access> reads;
  };

  static std::string key(const std::string& value) {
    const bool control = std::any_of(value.begin(), value.end(), [](char byte) {
      const auto c = static_cast<unsigned char>(byte);
      return c < 0x20 || c == 0x7f;
    });
    if (value.empty() || value.size() > 128 || control ||
        value.find_first_not_of(" ") == std::string::npos) {
      throw std::invalid_argument(
          "event identity must contain 1..128 non-control characters");
    }
    return value;
  }

  void budget() {
    if (events_ == 100000) throw std::logic_error("race analyzer event budget exhausted");
    ++events_;
  }

  Clock& clock(const std::string& participant) {
    const auto id = key(participant);
    auto it = clocks_.find(id);
    if (it != clocks_.end()) return it->second;
    if (clocks_.size() >= 256) {
      throw std::logic_error("race analyzer participant budget exhausted");
    }
    return clocks_.emplace(id, Clock{}).first->second;
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
    if (previous && !happens_before(previous->clock, current.clock)) {
      if (conflicts_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("race conflict count exhausted");
      }
      ++conflicts_;
      if (findings_.size() < 1000) {
        findings_.push_back(Finding{location, previous->participant,
                                    current.participant, kind});
      }
    }
  }

  mutable std::mutex mu_;
  std::size_t events_ = 0;
  std::uint64_t conflicts_ = 0;
  std::unordered_map<std::string, Clock> clocks_;
  std::unordered_map<std::string, Location> locations_;
  std::unordered_map<std::string, Clock> messages_;
  std::unordered_set<std::string> issued_message_ids_;
  std::unordered_map<std::string, Clock> locks_;
  std::unordered_map<std::string, std::string> lock_owners_;
  std::unordered_map<std::string, std::uint32_t> lock_depths_;
  std::vector<Finding> findings_;
};

}  // namespace oreslang::runtime
