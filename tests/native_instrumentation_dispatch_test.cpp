#include "oreslang/runtime/VersionedInstrumentationDispatch.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace oreslang::runtime;

static std::int64_t aot(void*, std::int64_t x) noexcept { return x + 1; }
static std::int64_t traced(void*, std::int64_t x) noexcept { return x + 100; }
static InstrumentationIdentity identity() {
    return {"tenant-a",std::string(32,'a'),std::string(64,'f'),
            std::string(64,'b'),std::string(64,'c'),std::string(64,'d'),std::string(64,'e')};
}
static std::shared_ptr<const InstrumentedVersion> make_version(std::uint64_t gen,
    NativeInstrumentedFn fn, bool aot_probe = false) {
    return std::make_shared<InstrumentedVersion>(InstrumentedVersion{identity(),gen,fn,{},nullptr,aot_probe});
}
struct BlockingContext {
    std::atomic<bool> entered{false};
    std::atomic<bool> exit_allowed{false};
    std::atomic<unsigned>* reclaimed;
    ~BlockingContext() { reclaimed->fetch_add(1, std::memory_order_release); }
};
static std::int64_t blocked(void* raw, std::int64_t x) noexcept {
    auto& ctx = *static_cast<BlockingContext*>(raw);
    ctx.entered.store(true, std::memory_order_release);
    while (!ctx.exit_allowed.load(std::memory_order_acquire)) std::this_thread::yield();
    return x + 200;
}

int main() {
    bool threw = false;
    try { VersionedInstrumentationDispatch invalid(nullptr,true,true,false); }
    catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
    auto baseline = make_version(1,aot);
    VersionedInstrumentationDispatch a(baseline,true,true,false);
    VersionedInstrumentationDispatch b(baseline,true,true,false);
    assert(a.invoke(41) == 42 && b.invoke(41) == 42);
    const auto v2 = make_version(2,traced);
    assert(a.publish(v2,InstrumentationMode::deep,false,true) == PublishResult::unauthorized);
    assert(a.publish(v2,InstrumentationMode::deep,true,false) == PublishResult::unauthorized);
    assert(a.publish(v2,InstrumentationMode::on,true,true) == PublishResult::unsupported);
    auto malformed = std::make_shared<InstrumentedVersion>(*v2);
    malformed->identity.ownership_sha256 = std::string(64,'f');
    assert(a.publish(malformed,InstrumentationMode::deep,true,true) == PublishResult::incompatible);
    auto foreign_tenant = std::make_shared<InstrumentedVersion>(*v2);
    foreign_tenant->identity.tenant_id = "tenant-b";
    assert(a.publish(foreign_tenant,InstrumentationMode::deep,true,true) == PublishResult::incompatible);
    auto different_ir = std::make_shared<InstrumentedVersion>(*v2);
    different_ir->identity.typed_ir_sha256 = std::string(64,'0');
    assert(a.publish(different_ir,InstrumentationMode::deep,true,true) == PublishResult::incompatible);
    assert(a.publish(v2,InstrumentationMode::deep,true,true) == PublishResult::accepted);
    assert(a.invoke(41) == 141 && b.invoke(41) == 42);
    assert(a.publish(v2,InstrumentationMode::deep,true,true) == PublishResult::stale_generation);
    a.disable();
    assert(a.invoke(41) == 42 && b.invoke(41) == 42);
    const auto v3 = make_version(3,traced,true);
    assert(a.publish(v3,InstrumentationMode::deep,true,true) == PublishResult::unsupported);
    assert(a.publish(v3,InstrumentationMode::on,true,true) == PublishResult::accepted);
    assert(a.invoke(41) == 141);
    a.disable();
    VersionedInstrumentationDispatch rt(baseline,true,false,true);
    assert(rt.publish(v2,InstrumentationMode::deep,true,true) == PublishResult::unsupported);
    assert(rt.publish(v3,InstrumentationMode::on,true,true) == PublishResult::accepted);
    assert(rt.invoke(41) == 141);

    // Pinning test: disable the variant while an in-flight native invocation
    // holds a lease; executable/context owner must outlive the invocation.
    std::atomic<unsigned> reclaimed{0};
    auto context = std::make_shared<BlockingContext>();
    context->reclaimed = &reclaimed;
    std::weak_ptr<BlockingContext> observer = context;
    std::shared_ptr<const InstrumentedVersion> waiting =
        std::make_shared<InstrumentedVersion>(InstrumentedVersion{
            identity(),4,blocked,context,context.get(),false});
    assert(a.publish(waiting,InstrumentationMode::deep,true,true) == PublishResult::accepted);
    std::atomic<std::int64_t> output{0};
    std::thread active_call([&] { output.store(a.invoke(10),std::memory_order_release); });
    while (!context->entered.load(std::memory_order_acquire)) std::this_thread::yield();
    a.disable(); waiting.reset();
    assert(!observer.expired() && reclaimed.load() == 0);
    context->exit_allowed.store(true, std::memory_order_release);
    context.reset();
    active_call.join();
    assert(output.load(std::memory_order_acquire) == 210);
    assert(observer.expired() && reclaimed.load() == 1);

    // Stress atomic pointer publication under concurrent callers. Outputs
    // must always correspond to a complete version, never a torn code slot.
    std::atomic<unsigned> bad{0};
    std::vector<std::thread> readers;
    for (unsigned n=0;n<4;++n) readers.emplace_back([&] {
        for (unsigned i=0;i<30000;++i) {
            const auto result = a.invoke(41);
            if (result != 42 && result != 141) bad.fetch_add(1);
        }
    });
    for (std::uint64_t gen=5;gen<505;++gen) {
        const auto v = make_version(gen,traced);
        assert(a.publish(v,InstrumentationMode::deep,true,true) == PublishResult::accepted);
        a.disable();
    }
    for (auto& reader : readers) reader.join();
    assert(bad.load() == 0 && a.invoke(41) == 42 && b.invoke(41) == 42);
    assert(a.publication_count() == 503 && b.publication_count() == 0);
    VersionedInstrumentationDispatch bounded(baseline,true,true,false,2);
    assert(bounded.publish(make_version(2,traced),InstrumentationMode::deep,true,true)
        == PublishResult::accepted);
    bounded.disable();
    assert(bounded.publish(make_version(3,traced),InstrumentationMode::deep,true,true)
        == PublishResult::accepted);
    bounded.disable();
    assert(bounded.publish(make_version(4,traced),InstrumentationMode::deep,true,true)
        == PublishResult::resource_limit);
    assert(bounded.publication_count() == 2 && bounded.active_generation() == 1);
    // Neither unauthorized nor incompatible candidates consume the budget.
    VersionedInstrumentationDispatch guarded(baseline,true,true,false,1);
    assert(guarded.publish(make_version(2,traced),InstrumentationMode::deep,false,true)
        == PublishResult::unauthorized);
    assert(guarded.publication_count() == 0);
    assert(guarded.publish(make_version(2,traced),InstrumentationMode::deep,true,true)
        == PublishResult::accepted);
    assert(guarded.publication_count() == 1);
    std::cout << "PASS native host-only tenant+typedIR-bound versioned dispatch, bounded publications, leases and races; NOT LLVM ORC or Atto guest execution\n";
}
