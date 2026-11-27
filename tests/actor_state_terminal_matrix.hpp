#pragma once

#include "oreslang/runtime/ActorStateReadWriteGate.hpp"
#include <algorithm>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

// Logical ticket retirement only; the scheduler must separately prove physical
// guest-turn quiescence before reclaiming memory.
inline void check_actor_state_terminal_matrix() {
    using Gate = oreslang::runtime::ActorStateReadWriteGate;
    auto check = [](bool ok) {
        if (!ok) throw std::runtime_error("actor state terminal matrix invariant failed");
    };
    for (auto mode : {Gate::Mode::Read, Gate::Mode::Write}) {
        for (int round = 0; round < 64; ++round) {
            Gate gate, independent;
            auto owner = gate.request(mode);
            auto waiter = gate.request(Gate::Mode::Write);
            auto later = gate.request(Gate::Mode::Read);
            auto other = independent.request(Gate::Mode::Write);
            check(owner.admission == Gate::Admission::acquired);
            check(waiter.admission == Gate::Admission::queued);
            check(later.admission == Gate::Admission::queued);
            std::atomic<bool> start{false};
            std::vector<Gate::Token> awakened, revoked;
            std::exception_ptr release_error, retire_error;
            std::thread release([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                try { awakened = gate.release(owner.token); }
                catch (...) { release_error = std::current_exception(); }
            });
            std::thread retire([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                try { revoked = gate.retire(); }
                catch (...) { retire_error = std::current_exception(); }
            });
            start.store(true, std::memory_order_release);
            release.join(); retire.join();
            if (release_error) std::rethrow_exception(release_error);
            if (retire_error) std::rethrow_exception(retire_error);
            check(std::count(revoked.begin(), revoked.end(), waiter.token) == 1);
            check(std::count(revoked.begin(), revoked.end(), later.token) == 1);
            check(revoked.size() == 2 || revoked.size() == 3);
            if (awakened.empty()) {
                check(std::count(revoked.begin(), revoked.end(), owner.token) == 1);
            } else {
                check(awakened.size() == 1 && awakened.front() == waiter.token);
                check(std::count(revoked.begin(), revoked.end(), owner.token) == 0);
            }
            for (auto token : {owner.token, waiter.token, later.token}) {
                check(!gate.holds(token, Gate::Mode::Read));
                check(!gate.holds(token, Gate::Mode::Write));
                check(gate.release(token).empty());
            }
            check(gate.retire().empty() && gate.waiting() == 0 && gate.active_readers() == 0);
            bool denied = false;
            try { (void)gate.request(mode); }
            catch (const std::logic_error&) { denied = true; }
            check(denied);
            check(independent.holds(other.token, Gate::Mode::Write));
            check(independent.release(other.token).empty());
        }
    }
    for (int round = 0; round < 128; ++round) {
        Gate gate;
        std::atomic<bool> start{false};
        Gate::Token ticket;
        std::vector<Gate::Token> revoked;
        bool denied = false;
        std::exception_ptr request_error, retire_error;
        std::thread request([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try { ticket = gate.request(Gate::Mode::Read).token; }
            catch (const std::logic_error&) { denied = true; }
            catch (...) { request_error = std::current_exception(); }
        });
        std::thread retire([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try { revoked = gate.retire(); }
            catch (...) { retire_error = std::current_exception(); }
        });
        start.store(true, std::memory_order_release);
        request.join(); retire.join();
        if (request_error) std::rethrow_exception(request_error);
        if (retire_error) std::rethrow_exception(retire_error);
        check(denied != static_cast<bool>(ticket));
        if (denied) check(revoked.empty());
        else {
            check(revoked.size() == 1 && revoked.front() == ticket);
            check(!gate.holds(ticket, Gate::Mode::Read));
            check(gate.release(ticket).empty());
        }
        check(gate.retire().empty() && gate.waiting() == 0 && gate.active_readers() == 0);
    }
}
