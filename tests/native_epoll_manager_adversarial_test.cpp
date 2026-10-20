#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>

using oreslang::runtime::NativeEpollReactor;
using oreslang::runtime::NativeEpollReactorManager;
static void require(bool good, const char* reason) {
    if (!good) {std::fprintf(stderr, "FAIL %s\n", reason); std::abort();}
}
static NativeEpollReactorManager::Completion wait_terminal(NativeEpollReactorManager& manager) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(std::chrono::steady_clock::now()<deadline) {
        auto done=manager.try_pop();
        if(done) return *done;
        std::this_thread::yield();
    }
    std::abort();
}
static void test_bounded_admission_and_ticket_recycling(){
    NativeEpollReactorManager manager(1);
    constexpr std::size_t capacity=NativeEpollReactor::capacity;
    std::array<std::array<int,2>,capacity+1> pipes{};
    std::array<NativeEpollReactorManager::Watch,capacity> watches{};
    require(manager.started_threads()==0,"throughput cold");
    for (auto& pipe : pipes) require(::pipe2(pipe.data(), O_NONBLOCK|O_CLOEXEC)==0,"pipe setup");
    for(std::size_t i=0;i<capacity;i++){
        auto token=manager.try_watch(pipes[i][0], EPOLLIN);
        require(token.has_value(),"admit up to capacity");
        watches[i]=*token;
        require(watches[i].ticket.value!=0,"nonzero token");
    }
    require(manager.started_threads()==1,"one reactor");
    require(!manager.try_watch(pipes[capacity][0],EPOLLIN),"capacity overflow rejected");
    require(!manager.try_watch(pipes[0][0],EPOLLIN),"same-fd duplicate denied");
    require(manager.try_cancel(watches[0]),"cancel frees one terminal only after pop");
    require(!manager.try_watch(pipes[capacity][0],EPOLLIN),"terminal retained cap");
    auto first=wait_terminal(manager);
    require(first.event.ticket.value==watches[0].ticket.value,"ticket unique first");
    require(first.event.outcome==NativeEpollReactor::Outcome::cancelled,"cancel outcome");
    auto replacement=manager.try_watch(pipes[capacity][0],EPOLLIN);
    require(replacement.has_value(),"slot reusable only after terminal");
    require(replacement->ticket.value!=watches[0].ticket.value,"no ticket ABA");
    require(!manager.try_cancel(watches[0]),"stale ticket denied");
    for(std::size_t i=1;i<capacity;i++) require(manager.try_cancel(watches[i]),"remaining cancel");
    require(manager.try_cancel(*replacement),"replacement cancel");
    std::array<bool,capacity> seen{};
    bool replacement_seen=false;
    for(std::size_t n=0;n<capacity;n++){
        auto done=wait_terminal(manager);
        require(done.event.outcome==NativeEpollReactor::Outcome::cancelled,"all cancel terminals");
        if(done.event.ticket.value==replacement->ticket.value){ require(!replacement_seen,"dup replacement"); replacement_seen=true; }
        else {
            bool matched=false;
            for(std::size_t i=1;i<capacity;i++) if(done.event.ticket.value==watches[i].ticket.value){require(!seen[i],"dup ticket");seen[i]=true;matched=true;break;}
            require(matched,"unknown terminal");
        }
    }
    for(std::size_t i=1;i<capacity;i++) require(seen[i],"all original watched terminals");
    require(replacement_seen,"replacement terminal");
    require(!manager.try_pop(),"queue empty");
    require(manager.orphaned_completions()==0,"all manager claims validated");
    for(auto& pipe:pipes){::close(pipe[0]);::close(pipe[1]);}
}
static void test_managed_eventfd_and_stop(){
    NativeEpollReactorManager manager(1);
    auto key=manager.open_owned_eventfd();
    require(key.has_value(),"mint managed kernel eventfd");
    auto teardown_key=manager.open_owned_eventfd();
    require(teardown_key.has_value(),"mint teardown eventfd before stop");
    require(manager.signal_owned_eventfd(*teardown_key),
            "pre-stop signal accepted for eventual cleanup");
    auto watch=manager.try_watch(*key,EPOLLIN);
    require(watch.has_value(),"admit authenticated managed eventfd");
    require(!manager.close_owned_eventfd(*key),"no close while claimed");
    require(manager.signal_owned_eventfd(*key),"signal managed eventfd");
    auto ready=wait_terminal(manager);
    require(ready.event.ticket.value==watch->ticket.value && ready.event.outcome==NativeEpollReactor::Outcome::ready,"managed READY");
    require(manager.drain_owned_eventfd(*key),"physical ack of eventfd counter");
    auto pending=manager.try_watch(*key,EPOLLIN);
    require(pending.has_value(),"managed rearm");
    manager.stop();
    require(!manager.signal_owned_eventfd(*key),
            "post-stop managed eventfd write must fail");
    require(!manager.signal_owned_eventfd(*teardown_key),
            "post-stop unclaimed eventfd write must fail");
    require(manager.drain_owned_eventfd(*teardown_key),
            "pre-stop counter may still drain after shutdown");
    require(!manager.drain_owned_eventfd(*teardown_key),
            "no phantom counter after one drain");
    require(manager.close_owned_eventfd(*teardown_key),
            "post-stop trusted cleanup of unclaimed eventfd");
    require(!manager.try_cancel(*pending),"cancel cannot steal shutdown");
    auto stopped=wait_terminal(manager);
    require(stopped.event.ticket.value==pending->ticket.value && stopped.event.outcome==NativeEpollReactor::Outcome::stopped,"stop exactly once");
    require(!manager.try_pop(),"no duplicate shutdown terminal");
    require(manager.close_owned_eventfd(*key),"close owned after terminal");
    require(!manager.close_owned_eventfd(*key),"double close forbidden");
    require(!manager.try_watch(*key,EPOLLIN),"stale handle denied");
    require(!manager.open_owned_eventfd(),"stopped manager denies new open");
    require(manager.orphaned_completions()==0,"no orphan after stop");
}
static void test_global_raw_stream_alias(){
    NativeEpollReactorManager managerA(1),managerB(1);
    int pipe[2];require(::pipe2(pipe,O_NONBLOCK|O_CLOEXEC)==0,"make stream");
    int alias=::dup(pipe[0]);require(alias>=0,"duplicate stream descriptor");
    auto a=managerA.try_watch(pipe[0],EPOLLIN);
    require(a.has_value(),"register initial stream");
    require(!managerB.try_watch(alias,EPOLLIN),"global alias claim denied");
    require(!managerB.try_cancel(*a),"foreign ticket cannot cancel");
    require(managerA.try_cancel(*a),"cancel first stream");
    auto terminal=wait_terminal(managerA);
    require(terminal.event.ticket.value==a->ticket.value,"validated terminal");
    auto b=managerB.try_watch(alias,EPOLLIN);
    require(b && b->ticket.value!=a->ticket.value,"stream transferred after release with new public ticket");
    require(!managerA.try_cancel(*b),"foreign manager rejected");
    require(managerB.try_cancel(*b),"cancel new stream");
    auto result=wait_terminal(managerB);
    require(result.event.ticket.value==b->ticket.value,"second terminal");
    ::close(alias);::close(pipe[0]);::close(pipe[1]);
}
int main(){
    test_bounded_admission_and_ticket_recycling();
    test_managed_eventfd_and_stop();
    test_global_raw_stream_alias();
    std::puts("NATIVE_MANAGER_ADVERSARIAL_PASS capacity=64 stop=1 claims=1");
}
