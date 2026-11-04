#include "oreslang/runtime/ScopedLocalStorageV1.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;

namespace {
ScopedLocalIdentityV1 id(std::uint64_t owner, std::uint64_t gen = 1) {
    return {11, 22, 33, owner, gen};
}

void actor_incarnation_and_domain_fences() {
    ActorLocalKeyV1<std::string> label;
    ActorLocalStoreV1 first(id(5)), restarted(id(5, 2));
    auto foreign_tenant = id(5); foreign_tenant.tenant = 44;
    auto foreign_isolate = id(5); foreign_isolate.isolate = 55;
    assert(first.set(label, id(5), std::string("actor-one")));
    assert(first.get_copy(label, id(5)) == std::string("actor-one"));
    assert(!first.get_copy(label, id(5, 2)));
    assert(!first.get_copy(label, foreign_tenant));
    assert(!first.set(label, foreign_isolate, std::string("wrong")));
    assert(!restarted.get_copy(label, id(5, 2)));
    assert(restarted.set(label, id(5, 2), std::string("actor-two")));
    assert(first.close());
    assert(!first.close());
    assert(!first.get_copy(label, id(5)));
    assert(!first.set(label, id(5), std::string("stale")));
    assert(restarted.get_copy(label, id(5, 2)) == std::string("actor-two"));
}

void logical_task_moves_without_inheriting_thread_local() {
    TaskLocalKeyV1<int> context;
    ThreadLocalKeyV1<int> native_tls;
    TaskLocalStoreV1 parent(id(71)), child(id(72));
    assert(parent.set(context, id(71), 100));
    std::thread thread_a([&] {
        assert(PhysicalThreadLocalStoreV1::set(native_tls, 41));
        assert(parent.get_copy(context, id(71)) == 100);
        assert(!child.get_copy(context, id(72)));
    });
    thread_a.join();
    std::thread thread_b([&] {
        assert(!PhysicalThreadLocalStoreV1::get_copy(native_tls));
        assert(parent.get_copy(context, id(71)) == 100);
        // Explicit copy is isolated; child tasks must not implicitly inherit.
        assert(child.set(context, id(72), *parent.get_copy(context, id(71))));
        assert(child.get_copy(context, id(72)) == 100);
        assert(PhysicalThreadLocalStoreV1::set(native_tls, 99));
    });
    thread_b.join();
    assert(!PhysicalThreadLocalStoreV1::get_copy(native_tls));
    assert(parent.set(context, id(71), 101));
    assert(child.get_copy(context, id(72)) == 100);
    assert(parent.close());
    assert(!parent.get_copy(context, id(71)));
    assert(child.get_copy(context, id(72)) == 100);
}

void physical_carrier_reuse_and_scrub() {
    ThreadLocalKeyV1<std::string> marker;
    assert(PhysicalThreadLocalStoreV1::set(marker, std::string("tenant-A")));
    // A different logical actor on SAME thread still observes native TLS.
    // Runtime/compiler must NEVER treat that as actor or tenant authority.
    assert(PhysicalThreadLocalStoreV1::get_copy(marker) == std::string("tenant-A"));
    PhysicalThreadLocalStoreV1::clear_current_thread();
    assert(!PhysicalThreadLocalStoreV1::get_copy(marker));
}

void exhaustion_and_atomic_replacement() {
    ActorLocalStoreV1 store(id(9));
    std::vector<ActorLocalKeyV1<int>> keys;
    for (int i=0; i<33; ++i) keys.emplace_back();
    for (int i=0; i<32; ++i) assert(store.set(keys[i], id(9), i));
    assert(!store.set(keys[32], id(9), 999));
    assert(store.get_copy(keys[31], id(9)) == 31);
    assert(store.set(keys[0], id(9), 902));
    assert(store.get_copy(keys[0], id(9)) == 902);
}

void close_racing_writer() {
    for (int n=0; n<100; ++n) {
        TaskLocalStoreV1 store(id(static_cast<std::uint64_t>(n+100)));
        TaskLocalKeyV1<int> key;
        std::atomic<bool> go{false};
        std::thread writer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i=0; i<100; ++i) {
                if (!store.set(key, id(static_cast<std::uint64_t>(n+100)), i)) break;
                (void)store.get_copy(key, id(static_cast<std::uint64_t>(n+100)));
            }
        });
        go.store(true, std::memory_order_release);
        store.close();
        writer.join();
        assert(!store.get_copy(key, id(static_cast<std::uint64_t>(n+100))));
        assert(!store.set(key, id(static_cast<std::uint64_t>(n+100)), 42));
    }
}

void byte_budget_replacement_removal_and_utf8() {
    ActorLocalStoreV1 a(id(55)), b(id(56));
    ActorLocalKeyV1<std::string> key;
    ActorLocalKeyV1<std::string> another;
    assert(a.set(key, id(55), std::string(8192, 'x')));
    assert(a.used_bytes() == 8192);
    assert(!a.set(key, id(55), std::string(8193, 'z')));
    assert(a.get_copy(key, id(55)) == std::string(8192, 'x'));
    assert(!a.set(another, id(55), std::string("over")));
    // Each actor has its own quota; size exhaustion is not process-global.
    assert(b.set(key, id(56), std::string(8192, 'b')));
    assert(a.set(key, id(55), std::string("abc")));
    assert(a.used_bytes() == 3);
    assert(a.set(another, id(55), std::string("\xC3\xA9")));
    assert(a.used_bytes() == 5); // UTF-8 bytes, not character count
    assert(!a.remove(key, id(56)));
    assert(a.remove(key, id(55)));
    assert(!a.remove(key, id(55)));
    assert(a.used_bytes() == 2);
    assert(a.remove(another, id(55)));
    assert(a.used_bytes() == 0);
    a.close();
    assert(!a.set(key, id(55), std::string("late")));
    assert(a.used_bytes() == 0);
}

void physical_thread_byte_budget_and_scrub() {
    ThreadLocalKeyV1<std::string> key;
    ThreadLocalKeyV1<int> counter;
    PhysicalThreadLocalStoreV1::clear_current_thread();
    assert(PhysicalThreadLocalStoreV1::set(key, std::string(8192, 'a')));
    assert(!PhysicalThreadLocalStoreV1::set(counter, 4));
    assert(!PhysicalThreadLocalStoreV1::set(key, std::string(8193, 'z')));
    assert(PhysicalThreadLocalStoreV1::get_copy(key) == std::string(8192, 'a'));
    assert(PhysicalThreadLocalStoreV1::set(key, std::string("small")));
    assert(PhysicalThreadLocalStoreV1::set(counter, 4));
    assert(PhysicalThreadLocalStoreV1::used_bytes() == 9);
    assert(PhysicalThreadLocalStoreV1::remove(counter));
    assert(!PhysicalThreadLocalStoreV1::remove(counter));
    assert(PhysicalThreadLocalStoreV1::used_bytes() == 5);
    PhysicalThreadLocalStoreV1::clear_current_thread();
    assert(PhysicalThreadLocalStoreV1::used_bytes() == 0);
    assert(!PhysicalThreadLocalStoreV1::get_copy(key));
}

void disallowed_unknown_dynamic_payload_fails_closed() {
    ActorLocalStoreV1 store(id(77));
    ActorLocalKeyV1<std::vector<int>> dynamic;
    assert(!store.set(dynamic, id(77), std::vector<int>{1, 2, 3}));
    // Trivially copyable does NOT imply safe: a record can embed a raw
    // pointer or host capability. Reject until transitive guest descriptors
    // and ownership checking exist for this type.
    struct PointerBearing {
        int* payload;
        unsigned int flags;
    };
    static_assert(std::is_trivially_copyable_v<PointerBearing>);
    int value = 42;
    ActorLocalKeyV1<PointerBearing> escaped;
    assert(!store.set(escaped, id(77), PointerBearing{&value, 1}));
    assert(!store.get_copy(escaped, id(77)));
    assert(store.used_bytes() == 0);
}

void malformed_utf8_is_rejected_without_mutation() {
    ActorLocalStoreV1 actor(id(88));
    ActorLocalKeyV1<std::string> key;
    assert(actor.set(key, id(88), std::string("original")));
    // Overlong, stray continuation, surrogate, >U+10FFFF, truncated
    // and valid 4-byte supplementary Unicode scalar.
    const std::string invalid[] = {
        std::string("\xC0\x80"),
        std::string("\x80"),
        std::string("\xED\xA0\x80"),
        std::string("\xF4\x90\x80\x80"),
        std::string("\xF0\x9F")
    };
    for (const auto& sample : invalid) {
        assert(!actor.set(key, id(88), sample));
        assert(actor.get_copy(key, id(88)) == std::string("original"));
        assert(actor.used_bytes() == 8);
    }
    assert(actor.set(key, id(88), std::string("\xF0\x9F\x98\x80")));
    assert(actor.used_bytes() == 4);
    ThreadLocalKeyV1<std::string> thread_key;
    PhysicalThreadLocalStoreV1::clear_current_thread();
    for (const auto& sample : invalid)
        assert(!PhysicalThreadLocalStoreV1::set(thread_key, sample));
    assert(PhysicalThreadLocalStoreV1::used_bytes() == 0);
}

void invalid_identity_denied() {
    bool threw=false;
    try { ActorLocalStoreV1 invalid({}); }
    catch (const std::invalid_argument&) { threw=true; }
    assert(threw);
}
}

int main() {
    actor_incarnation_and_domain_fences();
    logical_task_moves_without_inheriting_thread_local();
    physical_carrier_reuse_and_scrub();
    exhaustion_and_atomic_replacement();
    close_racing_writer();
    byte_budget_replacement_removal_and_utf8();
    physical_thread_byte_budget_and_scrub();
    disallowed_unknown_dynamic_payload_fails_closed();
    malformed_utf8_is_rejected_without_mutation();
    invalid_identity_denied();
    std::puts("SCOPED_LOCAL_HOST_ONLY_PASS");
}
