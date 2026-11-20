#include "oreslang/runtime/WalStorageFdOwner.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using oreslang::runtime::WalStorageFdOwner;
static int checks=0;
static void check(bool ok,const char* message) {++checks; if(!ok)throw std::runtime_error(message);}
template<typename E> static void reject(const std::function<void()>& f) {
    ++checks;
    try{f();}catch(const E&){return;}
    throw std::runtime_error("missing expected rejection");
}
static void le32(std::vector<std::uint8_t>& b,int offset,std::uint32_t n) {
    for(int i=0;i<4;++i)b.at(offset+i)=static_cast<std::uint8_t>(n>>(8*i));
}
static std::vector<std::uint8_t> frame(int tenant,int gen,int seq,int op) {
    std::vector<std::uint8_t> b(32,0);
    b[0]=79;b[1]=68;b[2]=66;b[3]=49;b[4]=1;b[5]=1;
    le32(b,6,tenant);le32(b,10,gen);le32(b,14,seq);le32(b,18,op);
    b[22]=1;b[24]=1;b[26]=107;b[27]=118;
    std::uint32_t a=1,c=0;
    for(int i=0;i<28;++i){a=(a+b[i])%65521;c=(c+a)%65521;}
    b[28]=a&255;b[29]=a>>8;b[30]=c&255;b[31]=c>>8;
    return b;
}
static void tests() {
    char name[]="/tmp/ores-wal-fd-owner-XXXXXX";
    int fd=::mkstemp(name);
    if(fd<0)throw std::runtime_error("mkstemp failed");
    int activation=7, foreign=7;
    WalStorageFdOwner::Scope scope{7,9,3,11,5};
    auto* wal=WalStorageFdOwner::adopt_verified_fd(fd,&activation,scope);
    reject<std::invalid_argument>([&]{wal->write_and_sync_on_disk_worker(&foreign,scope,frame(7,5,1,42));});
    auto stale=scope;stale.generation=6;
    reject<std::invalid_argument>([&]{wal->write_and_sync_on_disk_worker(&activation,stale,frame(7,5,1,42));});
    reject<std::invalid_argument>([&]{wal->write_and_sync_on_disk_worker(&activation,scope,frame(8,5,1,42));});
    auto damaged=frame(7,5,1,42);damaged[26]=108;
    reject<std::invalid_argument>([&]{wal->write_and_sync_on_disk_worker(&activation,scope,damaged);});
    WalStorageFdOwner::SyncedReceipt r1 = [&]() {
        std::vector<std::uint8_t> first=frame(7,5,1,42);
        // Physically run on a dedicated worker, not the guest carrier.
        // std::async propagates any native worker exception to this test.
        WalStorageFdOwner::SyncedReceipt out =
            std::async(std::launch::async, [&] {
                return wal->write_and_sync_on_disk_worker(&activation,scope,first);
            }).get();
        first.assign(first.size(),0);
        return out;
    }();
    check(r1.sequence()==1 && r1.operation()==42 && r1.end_offset()==32,"first synced receipt");
    check(r1.frame()==frame(7,5,1,42),"receipt holds copied exact frame");
    check(::lseek(fd,0,SEEK_END)==32,"actual file length");
    reject<std::invalid_argument>([&]{wal->write_and_sync_on_disk_worker(&activation,scope,frame(7,5,2,42));});
    reject<std::invalid_argument>([&]{wal->write_and_sync_on_disk_worker(&activation,scope,frame(7,5,3,44));});
    auto r2=wal->write_and_sync_on_disk_worker(&activation,scope,frame(7,5,2,43));
    check(r2.end_offset()==64 && r2.operation()==43,"ordered sync");
    wal->revoke();
    reject<std::logic_error>([&]{wal->write_and_sync_on_disk_worker(&activation,scope,frame(7,5,3,44));});
    delete wal;
    int reopened=::open(name,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    check(reopened>=0,"reopen physical WAL");
    if(reopened>=0) {
        struct stat st {};
        check(::fstat(reopened,&st)==0 && st.st_size==64,"synced physical length survives reopen");
        reject<std::invalid_argument>([&]{
            auto* not_ok=WalStorageFdOwner::adopt_verified_fd(reopened,&activation,scope);
            delete not_ok;
        });
        ::close(reopened);
    }
    ::unlink(name);
    // An exclusive hardlinked name must fail closed even for an empty FD.
    char other[]="/tmp/ores-wal-empty-XXXXXX";
    int empty=::mkstemp(other);
    if(empty<0)throw std::runtime_error("empty mkstemp failed");
    std::string linkname=std::string(other)+".linked";
    check(::link(other,linkname.c_str())==0,"create fixture hardlink");
    reject<std::invalid_argument>([&]{
        auto* not_ok=WalStorageFdOwner::adopt_verified_fd(empty,&activation,scope);
        delete not_ok;
    });
    ::close(empty);::unlink(linkname.c_str());::unlink(other);

    // Linux's pwrite() + O_APPEND can append instead of respecting the
    // explicit WAL sequence offset: reject this FD status at adoption.
    char appendname[]="/tmp/ores-wal-append-XXXXXX";
    int appendfd=::mkstemp(appendname);
    if(appendfd<0)throw std::runtime_error("append fixture setup failed");
    int current=::fcntl(appendfd,F_GETFL);
    check(current>=0 && ::fcntl(appendfd,F_SETFL,current|O_APPEND)==0,
          "enable unsafe O_APPEND fixture");
    reject<std::invalid_argument>([&]{
        auto* not_ok=WalStorageFdOwner::adopt_verified_fd(appendfd,&activation,scope);
        delete not_ok;
    });
    check(::fcntl(appendfd,F_GETFL)>=0,
          "rejected append FD remains caller-owned");
    ::close(appendfd);
    ::unlink(appendname);

    // A caller retaining an alias could flip O_APPEND after adoption.
    // Detect this before pwrite, quarantine and reject further writes.
    char changedname[]="/tmp/ores-wal-flags-change-XXXXXX";
    int changedfd=::mkstemp(changedname);
    if(changedfd<0)throw std::runtime_error("changed-flags fixture setup failed");
    auto* changed=WalStorageFdOwner::adopt_verified_fd(changedfd,&activation,scope);
    int initial_flags=::fcntl(changedfd,F_GETFL);
    check(initial_flags>=0 && ::fcntl(changedfd,F_SETFL,initial_flags|O_APPEND)==0,
          "mutate O_APPEND after WAL FD adoption");
    reject<WalStorageFdOwner::UnknownOutcome>([&]{
        changed->write_and_sync_on_disk_worker(&activation,scope,frame(7,5,1,99));
    });
    check(changed->quarantined(),"changed descriptor flags quarantine WAL owner");
    delete changed;
    ::unlink(changedname);

    char readname[]="/tmp/ores-wal-readonly-XXXXXX";
    int writable=::mkstemp(readname);
    if(writable<0)throw std::runtime_error("read-only fixture setup failed");
    ::close(writable);
    int readonly=::open(readname,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    check(readonly>=0,"open read-only WAL fixture");
    auto* failed=WalStorageFdOwner::adopt_verified_fd(readonly,&activation,scope);
    reject<WalStorageFdOwner::UnknownOutcome>([&]{
        failed->write_and_sync_on_disk_worker(&activation,scope,frame(7,5,1,1));
    });
    check(failed->quarantined(),"I/O failure quarantines native owner");
    reject<std::logic_error>([&]{
        failed->write_and_sync_on_disk_worker(&activation,scope,frame(7,5,2,2));
    });
    delete failed;::unlink(readname);
}
int main() {
    try{tests();std::cout<<"WAL_NATIVE_HOST_FD_PASS checks="<<checks<<"; NO GUEST OR POWER-LOSS CLAIM\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
