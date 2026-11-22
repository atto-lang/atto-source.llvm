#include "oreslang/backend/GpuG1EventProvider.hpp"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace oreslang::gpu::g1;
namespace {
std::uint64_t assertions = 0;
void check(bool b) { ++assertions; if (!b) std::abort(); }
template<class F> void rejects(F f) {
    bool yes = false;
    try { f(); } catch (const std::invalid_argument&) { yes = true; }
    check(yes);
}

// Fake is TEST_ONLY, never an authenticated hardware source.
struct FakeCuda final {
    using Kernel = std::uint64_t;
    using Stream = std::uint64_t;
    using Buffer = std::uint64_t;
    using Event = std::uint64_t;
    struct Limits { std::uint32_t maximum_threads_per_block; std::uint64_t maximum_grid_x; };
    struct EventInfo { bool recorded = false; bool completed = false; };
    std::mutex lock;
    std::map<Event, EventInfo> events;
    std::map<Stream, bool> streams;
    std::map<Buffer, std::size_t> buffers;
    std::uint64_t next_stream=1, next_buffer=4096, next_event=1;
    std::uint64_t launches=0, recorded=0, event_queries=0;
    bool fail_launch=false, fail_record=false, fail_query=false, fail_destroy_event=false;
    Limits limits() const { return {256, 65535}; }
    bool valid_kernel(Kernel kernel) const { return kernel==123; }
    Stream create_stream() {
        std::lock_guard<std::mutex> g(lock);
        const auto id=next_stream++;
        streams.emplace(id,true);
        return id;
    }
    void destroy_stream(Stream s) {
        std::lock_guard<std::mutex> g(lock);
        if(streams.erase(s)!=1)throw std::invalid_argument("fake invalid stream");
    }
    Buffer allocate(std::size_t bytes) {
        std::lock_guard<std::mutex> g(lock);
        auto id=next_buffer;
        next_buffer+=bytes+256;
        buffers.emplace(id,bytes);
        return id;
    }
    void free(Buffer id) {
        std::lock_guard<std::mutex> g(lock);
        if(buffers.erase(id)!=1)throw std::invalid_argument("fake double-free");
    }
    Event create_event() {
        std::lock_guard<std::mutex> g(lock);
        auto id=next_event++;
        events.emplace(id,EventInfo{});
        return id;
    }
    void destroy_event(Event id) {
        std::lock_guard<std::mutex> g(lock);
        if(fail_destroy_event || events.erase(id)!=1)throw std::invalid_argument("fake destroy event fail");
    }
    Buffer offset_ptr(Buffer id,std::uint64_t off) {
        std::lock_guard<std::mutex> g(lock);
        const auto it=buffers.upper_bound(id);
        if(it==buffers.begin())throw std::invalid_argument("fake invalid ptr");
        const auto prev=std::prev(it);
        if(off>prev->second)throw std::invalid_argument("fake off bounds");
        return id+off;
    }
    void launch_saxpy(Kernel kernel,Stream stream,float scale,Buffer x,Buffer y,Buffer out,
                      std::uint64_t n,std::uint32_t grid,std::uint32_t block) {
        std::lock_guard<std::mutex> g(lock);
        if(fail_launch)throw std::invalid_argument("fake launch fail");
        if(kernel!=123||!streams.count(stream)||!std::isfinite(scale)||!x||!y||!out||
           !n||!grid||!block)throw std::invalid_argument("fake invalid kernel call");
        ++launches;
    }
    void record_event(Event event,Stream stream) {
        std::lock_guard<std::mutex> g(lock);
        if(fail_record)throw std::invalid_argument("fake record fail");
        if(!streams.count(stream)||!events.count(event))throw std::invalid_argument("fake bad event");
        events.at(event).recorded=true;
        ++recorded;
    }
    HardwareEventState query_event(Event event) {
        std::lock_guard<std::mutex> g(lock);
        ++event_queries;
        if(fail_query)return HardwareEventState::DeviceError;
        auto it=events.find(event);
        if(it==events.end()||!it->second.recorded)return HardwareEventState::DeviceError;
        return it->second.completed?HardwareEventState::Completed:HardwareEventState::Pending;
    }
    void complete_last() {
        std::lock_guard<std::mutex> g(lock);
        if(events.empty())throw std::invalid_argument("fake no event");
        events.rbegin()->second.completed=true;
    }
    std::size_t active_events() {
        std::lock_guard<std::mutex> g(lock);return events.size();
    }
};
using Provider = GpuG1EventProvider<FakeCuda>;
using Ledger = DeviceLeaseRegistry;
const Ledger::Limits big{1<<20,256,64,16,1<<18,512,512};

void basic_lifecycle() {
    Ledger ledger(big);FakeCuda driver;Provider provider(ledger,driver,1,2);
    auto q=provider.create_queue();
    auto x=provider.allocate(1024), y=provider.allocate(1024), out=provider.allocate(1024);
    const Provider::Launch kernel{q,123,{x,0},{y,0},{out,0},2.0f,255,256};
    auto t=provider.launch_saxpy(kernel);
    check(t!=0 && provider.pending_events()==1);
    check(driver.launches==1 && driver.recorded==1);
    check(provider.poll(q,t)==DispatchState::Pending);
    rejects([&]{provider.release_allocation(x);});
    rejects([&]{provider.close_queue(q);});
    provider.request_cancel(q,t);
    check(provider.poll(q,t)==DispatchState::Pending);
    check(ledger.snapshot().cancelled_pending==1);
    rejects([&]{provider.release_allocation(out);});
    driver.complete_last();
    check(provider.poll(q,t)==DispatchState::CancelledQuiescent);
    check(provider.pending_events()==0 && driver.active_events()==0);
    check(ledger.snapshot().inflight==0);
    rejects([&]{provider.poll(q,t);});
    rejects([&]{provider.request_cancel(q,t);});
    auto t2=provider.launch_saxpy(kernel);
    driver.complete_last();
    check(provider.poll(q,t2)==DispatchState::Completed);
    const auto calls=driver.launches;
    auto empty=kernel;empty.n=0;
    check(provider.launch_saxpy(empty)==0 && driver.launches==calls && driver.active_events()==0);
    auto incorrect=kernel;incorrect.block_threads=0;
    rejects([&]{(void)provider.launch_saxpy(incorrect);});
    incorrect=kernel;incorrect.block_threads=257;
    rejects([&]{(void)provider.launch_saxpy(incorrect);});
    incorrect=kernel;incorrect.scale=std::numeric_limits<float>::infinity();
    rejects([&]{(void)provider.launch_saxpy(incorrect);});
    incorrect=kernel;incorrect.kernel=0;
    rejects([&]{(void)provider.launch_saxpy(incorrect);});
    incorrect=kernel;incorrect.x.offset_bytes=2;
    rejects([&]{(void)provider.launch_saxpy(incorrect);});
    incorrect=kernel;incorrect.x.offset_bytes=1020;
    rejects([&]{(void)provider.launch_saxpy(incorrect);});
    incorrect=kernel;incorrect.n=UINT64_MAX;
    rejects([&]{(void)provider.launch_saxpy(incorrect);});
    incorrect=kernel;incorrect.out.allocation=x;
    rejects([&]{(void)provider.launch_saxpy(incorrect);});
    incorrect=kernel;incorrect.y.allocation=x;
    auto reads=provider.launch_saxpy(incorrect);
    driver.complete_last();
    check(provider.poll(q,reads)==DispatchState::Completed);
    check(ledger.snapshot().inflight==0);
    Provider::Launch disjoint=kernel;
    disjoint.x.offset_bytes=0;disjoint.x.allocation=x;
    disjoint.y.offset_bytes=0;disjoint.y.allocation=y;
    disjoint.out.offset_bytes=512;disjoint.out.allocation=x;
    disjoint.n=128;
    const auto td=provider.launch_saxpy(disjoint);
    driver.complete_last();check(provider.poll(q,td)==DispatchState::Completed);
    provider.release_allocation(x);provider.release_allocation(y);provider.release_allocation(out);
    provider.close_queue(q);
    provider.require_quiescent_shutdown();
    check(ledger.snapshot().resident_bytes==0);
    check(driver.buffers.empty() && driver.streams.empty());
}

void multi_provider_and_quarantine() {
    Ledger ledger(big);FakeCuda driver;
    Provider provider(ledger,driver,1,2),different(ledger,driver,2,2),elsewhere(ledger,driver,1,3);
    auto q=provider.create_queue();auto x=provider.allocate(512),y=provider.allocate(512),z=provider.allocate(512);
    auto q2=different.create_queue();auto other=different.allocate(512);
    auto q3=elsewhere.create_queue();auto safe=elsewhere.allocate(512);
    Provider::Launch kernel{q,123,{x,0},{y,0},{z,0},1.0f,8,16};
    rejects([&]{(void)provider.launch_saxpy({q2,123,{x,0},{y,0},{z,0},1.0f,8,16});});
    rejects([&]{(void)provider.launch_saxpy({q,123,{other,0},{y,0},{z,0},1.0f,8,16});});
    const auto ticket=provider.launch_saxpy(kernel);
    check(ticket != 0);
    check(provider.poll(q,ticket)==DispatchState::Pending);
    provider.quarantine_device();
    check(provider.is_quarantined());
    check(ledger.snapshot().inflight==1);
    check(provider.poll(q,ticket)==DispatchState::DeviceLost);
    rejects([&]{provider.release_allocation(z);});
    rejects([&]{provider.close_queue(q);});
    rejects([&]{(void)provider.create_queue();});
    rejects([&]{(void)provider.allocate(32);});
    rejects([&]{(void)different.create_queue();});
    rejects([&]{(void)different.allocate(32);});
    // A distinct device domain survives the quarantine.
    auto safe2=elsewhere.allocate(512);
    check(safe2.generation() > 0);
    elsewhere.release_allocation(safe2);
    elsewhere.release_allocation(safe);
    elsewhere.close_queue(q3);
    elsewhere.require_quiescent_shutdown();
    check(ledger.snapshot().inflight==1);
}

void driver_failures_and_concurrent_writers() {
    {Ledger ledger(big);FakeCuda driver;Provider provider(ledger,driver,1,2);
     auto q=provider.create_queue();auto x=provider.allocate(1024),y=provider.allocate(1024),z=provider.allocate(1024);
     Provider::Launch kernel{q,123,{x,0},{y,0},{z,0},1.0f,7,8};
     driver.fail_record=true;
     rejects([&]{(void)provider.launch_saxpy(kernel);});
     check(provider.is_quarantined() && provider.pending_events()==1);
     check(ledger.snapshot().inflight==1);
     rejects([&]{provider.release_allocation(z);});
    }
    {Ledger ledger(big);FakeCuda driver;Provider provider(ledger,driver,1,2);
     auto q=provider.create_queue();auto x=provider.allocate(1024),y=provider.allocate(1024),z=provider.allocate(1024);
     Provider::Launch kernel{q,123,{x,0},{y,0},{z,0},1.0f,7,8};
     driver.fail_launch=true;
     rejects([&]{(void)provider.launch_saxpy(kernel);});
     check(provider.is_quarantined() && ledger.snapshot().inflight==1);
    }
    {Ledger ledger(big);FakeCuda driver;Provider provider(ledger,driver,1,2);
     auto q=provider.create_queue();auto x=provider.allocate(1024),y=provider.allocate(1024),z=provider.allocate(1024);
     Provider::Launch kernel{q,123,{x,0},{y,0},{z,0},1.0f,7,8};
     auto t=provider.launch_saxpy(kernel);
     driver.fail_query=true;
     check(provider.poll(q,t)==DispatchState::DeviceLost);
     check(provider.is_quarantined() && ledger.snapshot().inflight==1);
     check(provider.poll(q,t)==DispatchState::DeviceLost);
    }
    {Ledger ledger(big);FakeCuda driver;Provider provider(ledger,driver,1,2);
     auto q=provider.create_queue();auto x=provider.allocate(1024),y=provider.allocate(1024),z=provider.allocate(1024);
     Provider::Launch kernel{q,123,{x,0},{y,0},{z,0},1.0f,7,8};
     std::atomic<int> winner{0},denied{0};
     std::atomic<std::uint64_t> ticket{0};
     std::atomic<bool> start{false};std::vector<std::thread> workers;
     for(int i=0;i<16;i++)workers.emplace_back([&]{
         while(!start.load(std::memory_order_acquire))std::this_thread::yield();
         try { auto t=provider.launch_saxpy(kernel);ticket.store(t);winner.fetch_add(1); }
         catch(const std::invalid_argument&) { denied.fetch_add(1); }
     });
     start.store(true,std::memory_order_release);
     for(auto& w:workers)w.join();
     check(winner==1 && denied==15 && provider.pending_events()==1);
     driver.complete_last();
     check(provider.poll(q,ticket.load())==DispatchState::Completed);
     provider.release_allocation(z);provider.release_allocation(x);provider.release_allocation(y);
     provider.close_queue(q);provider.require_quiescent_shutdown();
    }
}

void randomized_completion_and_cancel() {
    Ledger ledger(big);FakeCuda driver;Provider provider(ledger,driver,1,2);
    auto q=provider.create_queue();auto x=provider.allocate(4096),y=provider.allocate(4096),z=provider.allocate(4096);
    std::uint32_t seed=0x891244ff;
    for(int i=0;i<512;++i){
        seed=1664525u*seed+1013904223u;
        const std::uint64_t n=seed%513;
        seed=1664525u*seed+1013904223u;
        const std::uint32_t group=1+seed%256;
        Provider::Launch k{q,123,{x,0},{y,0},{z,0},1.25f,n,group};
        auto ticket=provider.launch_saxpy(k);
        if(n==0){check(ticket==0);continue;}
        check(ticket>0);
        check(provider.poll(q,ticket)==DispatchState::Pending);
        if(i%3==0){provider.request_cancel(q,ticket);check(provider.poll(q,ticket)==DispatchState::Pending);}
        rejects([&]{provider.release_allocation(z);});
        driver.complete_last();
        const auto status=provider.poll(q,ticket);
        check(status==(i%3==0?DispatchState::CancelledQuiescent:DispatchState::Completed));
        check(ledger.snapshot().inflight==0);
        check(provider.pending_events()==0);
    }
    provider.release_allocation(x);provider.release_allocation(y);provider.release_allocation(z);
    provider.close_queue(q);provider.require_quiescent_shutdown();
}
}
int main() { basic_lifecycle(); multi_provider_and_quarantine(); driver_failures_and_concurrent_writers(); randomized_completion_and_cancel();std::cout<<"PASS "<<assertions<<" host-only event provider assertions\n"; }
