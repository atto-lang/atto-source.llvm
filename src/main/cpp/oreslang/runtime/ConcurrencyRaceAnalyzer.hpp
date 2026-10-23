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
 *
 * Participant IDs MUST be per independently executing logical task/message
 * lifetime, including separate simultaneous lanes inside a multi-thread actor.
 * Actor reference IDs and carrier thread IDs are not sound participant IDs:
 * the former merges concurrent clocks, the latter breaks on carrier migration.
 * Complete compiler/runtime instrumentation must enforce participant identity.
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
    const auto id = key(location);
    clock(participant);
    budget();
    const auto access = tick(participant);
    auto& state = locations_[id];
    for (const auto& writer : state.writes) {
      conflict(id, std::optional<Access>(writer.second), access, "write-read");
    }
    state.reads[participant] = access;
  }

  void write(const std::string& participant, const std::string& location) {
    std::lock_guard<std::mutex> hold(mu_);
    const auto id = key(location);
    clock(participant);
    budget();
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
    const auto id = key(message_id);
    const auto actor = key(participant);
    if (issued_message_ids_.count(id) != 0) {
      throw std::logic_error("message token cannot be reused, including after receive");
    }
    clock(actor);
    budget();
    const auto published = tick(actor).clock;
    issued_message_ids_.insert(id);
    messages_.emplace(id, published);
  }

  void receive(const std::string& participant, const std::string& message_id) {
    std::lock_guard<std::mutex> hold(mu_);
    const auto id = key(message_id);
    const auto actor = key(participant);
    auto it = messages_.find(id);
    if (it == messages_.end()) {
      throw std::logic_error("message token not published");
    }
    auto& destination = clock(actor);
    budget();
    merge(destination, it->second);
    tick(actor);
    messages_.erase(it);
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
    clock(actor);
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
    clock(actor);
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

  // Child is a fresh logical lifetime: it inherits ONLY pre-fork events.
  void fork(const std::string& parent, const std::string& child) {
    std::lock_guard<std::mutex> hold(mu_);
    const auto ancestor = key(parent);
    const auto descendant = key(child);
    if (ancestor == descendant || clocks_.count(descendant) != 0 ||
        fork_parents_.count(descendant) != 0 ||
        finished_participants_.count(descendant) != 0) {
      throw std::logic_error("fork requires a fresh child participant");
    }
    clock(ancestor);
    if (clocks_.size() >= 256) {
      throw std::logic_error("race analyzer participant budget exhausted");
    }
    budget();
    Clock inherited = tick(ancestor).clock;
    inherited[descendant] = 1;
    clocks_.emplace(descendant, std::move(inherited));
    fork_parents_.emplace(descendant, ancestor);
  }

  void finish(const std::string& participant) {
    std::lock_guard<std::mutex> hold(mu_);
    const auto child = key(participant);
    if (clocks_.find(child) == clocks_.end() ||
        finished_participants_.count(child) != 0) {
      throw std::logic_error("finish requires one live participant");
    }
    for (const auto& held : lock_owners_) {
      if (held.second == child) {
        throw std::logic_error("cannot finish while owning a lock");
      }
    }
    budget();
    const auto completed = tick(child).clock;
    finished_participants_.insert(child);
    if (fork_parents_.count(child) != 0) {
      completed_children_.emplace(child, completed);
    }
  }

  // Only a finished direct child can synchronize with its parent, once.
  void join(const std::string& parent, const std::string& child) {
    std::lock_guard<std::mutex> hold(mu_);
    const auto ancestor = key(parent);
    const auto descendant = key(child);
    const auto origin = fork_parents_.find(descendant);
    const auto completed = completed_children_.find(descendant);
    if (origin == fork_parents_.end() || origin->second != ancestor ||
        completed == completed_children_.end() ||
        joined_children_.count(descendant) != 0) {
      throw std::logic_error("join requires a finished direct child, exactly once");
    }
    auto& destination = clock(ancestor);
    budget();
    merge(destination, completed->second);
    tick(ancestor);
    joined_children_.insert(descendant);
    completed_children_.erase(completed);
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
    if (events_ >= 100000) throw std::logic_error("race analyzer event budget exhausted");
    ++events_;
  }

  Clock& clock(const std::string& participant) {
    const auto id = key(participant);
    if (finished_participants_.count(id) != 0) {
      throw std::logic_error("participant lifetime already finished");
    }
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
  std::unordered_map<std::string, std::string> fork_parents_;
  std::unordered_map<std::string, Clock> completed_children_;
  std::unordered_set<std::string> finished_participants_;
  std::unordered_set<std::string> joined_children_;
  std::vector<Finding> findings_;
};

}  // namespace oreslang::runtime
