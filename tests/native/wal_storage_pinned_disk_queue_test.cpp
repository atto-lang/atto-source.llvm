#include "oreslang/runtime/WalStoragePinnedDiskQueue.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace oreslang::runtime;
int checks=0;
void ok(bool truth, const char* what) {
    ++checks;
    if (!truth) throw std::runtime_error(what);
}
template<class Err, class Action>
void rejects(Action&& action, const char* message) {
    ++checks;
    try { action(); } catch (const Err&) { return; }
    throw std::runtime_error(message);
}
std::vector<std::uint8_t> wal_frame(std::uint32_t tenant,
                                    std::uint32_t generation,
                                    std::uint32_t sequence,
                                    std::uint32_t operation) {
    std::vector<std::uint8_t> b(32,0);
    b[0]=79; b[1]=68; b[2]=66; b[3]=49; b[4]=1; b[5]=1;
    auto word=[&](int offset,std::uint32_t value) {
        for(int i=0;i<4;++i) b[static_cast<std::size_t>(offset+i)]=
            static_cast<std::uint8_t>(value>>(8*i));
    };
    word(6,tenant); word(10,generation); word(14,sequence); word(18,operation);
    b[22]=1; b[24]=1; b[26]='k'; b[27]='v';
    std::uint32_t a=1,c=0;
    for(std::size_t i=0;i<28;++i) {a=(a+b[i])%65521;c=(c+a)%65521;}
    b[28]=static_cast<std::uint8_t>(a);b[29]=static_cast<std::uint8_t>(a>>8);
    b[30]=static_cast<std::uint8_t>(c);b[31]=static_cast<std::uint8_t>(c>>8);
    return b;
}
struct TempFile {
    int fd=-1;
    std::string path;
    TempFile() {
        char name[]="/tmp/ores-wal-native-pin-XXXXXX";
        fd=::mkstemp(name);
        if(fd<0) throw std::runtime_error("mkstemp failed");
        path=name;
    }
    ~TempFile() { if(fd>=0) (void)::close(fd); (void)::unlink(path.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    int transfer() {const int out=fd; fd=-1;return out;}
};
}
int main() {
    NativeActorGroup group(9,1);
    auto actor=group.adopt(9,11,5,[]{});
    auto finalizer=group.runtime_finalizer(actor);
    ok(group.mark_ready(actor),"native actor ready");
    const WalStorageFdOwner::Scope scope{7,9,3,11,5};
    int grant=7;
    TempFile temp;
    auto owner=std::unique_ptr<WalStorageFdOwner>(
        WalStorageFdOwner::adopt_verified_fd(temp.transfer(),&grant,scope));
    auto queue=WalStoragePinnedDiskQueue::adopt_from_isolate(
        actor,std::move(owner),&grant,scope);
    ok(actor.outstanding_resource_loans()==1,"native actor resource loan retained");
    auto frame=wal_frame(7,5,1,42);
    auto first=queue->submit(actor,&grant,scope,frame);
    ok(first.get().operation()==42,"real FD append and fdatasync");
    auto second=queue->submit(actor,&grant,scope,wal_frame(7,5,2,43));
    ok(second.get().sequence()==2,"second ordered native WAL receipt");
    rejects<std::invalid_argument>([&]{
        queue->submit(actor,&grant,WalStorageFdOwner::Scope{7,9,3,11,6},
                      wal_frame(7,5,3,44));
    },"foreign scope accepted");
    NativeActorGroup foreignGroup(10,1);
    auto foreign=foreignGroup.adopt(10,11,5,[]{});
    rejects<std::invalid_argument>([&]{
        queue->submit(foreign,&grant,scope,wal_frame(7,5,3,44));
    },"equal actor ID but foreign runtime ticket accepted");
    // Same numeric actor ID and generation is not the same opaque incarnation.
    ok(actor.resource_admission_open(),"active native actor admission open");
    group.close();
    ok(!actor.resource_admission_open(),"stopped native actor admission closed");
    rejects<std::logic_error>([&]{
        queue->submit(actor,&grant,scope,wal_frame(7,5,3,44));
    },"closed actor group accepted new WAL disk effect");
    ok(!finalizer.ready_for_resource_reclamation(),
       "actor arena could retire while native WAL queue still owns resources");
    queue.reset(); // only off-carrier shutdown; joins workers before loan release
    ok(actor.outstanding_resource_loans()==0,"loan released after physical quiescence");
    ok(finalizer.ready_for_resource_reclamation(),"actor arena retirement unblocked");
    ok(finalizer.finalized(),"native actor completed after owned FD retirement");
    // Test stale/closed incarnation refuses new loan creation.
    TempFile other;
    auto secondOwner=std::unique_ptr<WalStorageFdOwner>(
        WalStorageFdOwner::adopt_verified_fd(other.transfer(),&grant,scope));
    rejects<std::logic_error>([&]{
        WalStoragePinnedDiskQueue::adopt_from_isolate(
            actor,std::move(secondOwner),&grant,scope);
    },"finalized actor retained new WAL FD loan");
    foreignGroup.close();
    std::cout<<"WAL_NATIVE_PINNED_QUEUE_PASS checks="<<checks
             <<"; NO GUEST OR POWER-LOSS PROOF\n";
}
